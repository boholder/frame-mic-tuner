// 外部コマンドの実行の実装。
#include "command.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace {

/**
 * 単調増加の時計で今の時刻を ms で返す。
 * @return ms
 */
long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/**
 * パイプから読めるだけ読んで足す。
 * @param fd 読むパイプ（非ブロッキング）
 * @param text 追加先
 * @return まだ開いている（EOF でない）なら true
 */
bool drain(int fd, std::string& text) {
    char buffer[4096];
    while (true) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            // 何かの間違いで出力が巨大でも、メモリを食い尽くさないよう上限を設ける
            if (text.size() < 1024 * 1024) text.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

/**
 * パイプの両端を閉じる（開いているものだけ）。
 * @param fds パイプ
 */
void closePipe(int fds[2]) {
    for (int i = 0; i < 2; ++i) {
        if (fds[i] >= 0) ::close(fds[i]);
        fds[i] = -1;
    }
}

}  // namespace

CommandResult runCommand(const std::vector<std::string>& argv, int timeoutMs) {
    CommandResult result;
    if (argv.empty()) return result;

    // exec に渡す配列は fork の前に作っておく（子プロセスではメモリを確保しない）
    std::vector<char*> args;
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);

    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (::pipe2(outPipe, O_CLOEXEC) != 0 || ::pipe2(errPipe, O_CLOEXEC) != 0) {
        result.err = std::string("pipe: ") + std::strerror(errno);
        closePipe(outPipe);
        closePipe(errPipe);
        return result;
    }
    const int devNull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

    const pid_t pid = ::fork();
    if (pid < 0) {
        result.err = std::string("fork: ") + std::strerror(errno);
        closePipe(outPipe);
        closePipe(errPipe);
        if (devNull >= 0) ::close(devNull);
        return result;
    }
    if (pid == 0) {
        // 子プロセス: ここでは async-signal-safe な呼び出しだけを使う
        if (devNull >= 0) ::dup2(devNull, STDIN_FILENO);
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(errPipe[1], STDERR_FILENO);
        // 呼び出したスレッドがシグナルを止めていても、子プロセスには持ち越さない
        sigset_t empty;
        sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);  // exec できなかった
    }

    // 親プロセス: 書き込み側を閉じて、両方の出力を時間切れまで読む
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    outPipe[1] = -1;
    errPipe[1] = -1;
    if (devNull >= 0) ::close(devNull);
    ::fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    ::fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    const long long deadline = nowMs() + timeoutMs;
    bool outOpen = true;
    bool errOpen = true;
    bool timedOut = false;
    while (outOpen || errOpen) {
        const long long left = deadline - nowMs();
        if (left <= 0) {
            timedOut = true;
            break;
        }
        pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
        if (!outOpen) fds[0].fd = -1;  // 閉じたほうは poll で見ない
        if (!errOpen) fds[1].fd = -1;
        const int ready = ::poll(fds, 2, static_cast<int>(left));
        if (ready < 0) {
            if (errno == EINTR) continue;
            timedOut = true;  // poll が使えないときも、待ち続けずに止める
            break;
        }
        if (outOpen && fds[0].revents != 0) outOpen = drain(outPipe[0], result.out);
        if (errOpen && fds[1].revents != 0) errOpen = drain(errPipe[0], result.err);
    }
    if (timedOut) ::kill(pid, SIGKILL);
    closePipe(outPipe);
    closePipe(errPipe);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timedOut) {
        result.status = CommandResult::Status::Timeout;
    } else if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
        result.status = result.exitCode == 0 ? CommandResult::Status::Ok : CommandResult::Status::Failed;
        if (result.exitCode == 127 && result.out.empty() && result.err.empty()) {
            result.status = CommandResult::Status::SpawnFailed;
            result.err = "找不到命令";
        }
    } else {
        result.status = CommandResult::Status::Failed;
    }
    return result;
}

std::string describeCommand(const std::vector<std::string>& argv, const CommandResult& result) {
    std::string line;
    for (const auto& arg : argv) line += (line.empty() ? "" : " ") + arg;
    switch (result.status) {
        case CommandResult::Status::Ok: line += " -> 成功"; break;
        case CommandResult::Status::Failed:
            line += result.exitCode >= 0 ? " -> 退出码 " + std::to_string(result.exitCode) : " -> 被信号终止";
            break;
        case CommandResult::Status::Timeout: line += " -> 超时已停止"; break;
        case CommandResult::Status::SpawnFailed: line += " -> 无法启动"; break;
    }
    std::string err = result.err;
    while (!err.empty() && (err.back() == '\n' || err.back() == ' ')) err.pop_back();
    const size_t newline = err.find('\n');
    if (newline != std::string::npos) err = err.substr(0, newline);  // 1 行目だけ
    if (!err.empty()) line += ": " + err;
    return line;
}
