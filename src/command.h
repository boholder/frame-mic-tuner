// 外部コマンド（wpctl・pw-link・systemctl）を、シェルを通さずに実行して出力を受け取る。
#pragma once

#include <string>
#include <vector>

/** runCommand() の結果。 */
struct CommandResult {
    enum class Status {
        Ok,           ///< 終了コード 0 で終わった
        Failed,       ///< 0 以外の終了コード・シグナルで終わった
        Timeout,      ///< 時間内に終わらなかったので SIGKILL で止めた
        SpawnFailed,  ///< 起動できなかった（パイプ・fork・exec の失敗）
    };
    Status status = Status::SpawnFailed;
    int exitCode = -1;       ///< 終了コード（シグナルで終わったときは -1）
    std::string out;         ///< 標準出力
    std::string err;         ///< 標準エラー（ログ用）

    /** @return 終了コード 0 で終わったら true */
    bool ok() const { return status == Status::Ok; }
};

/**
 * コマンドを fork＋execvp で実行し、終わるまで待って出力を返す（シェルは通さない）。
 * 時間内に終わらなければ SIGKILL で止め、どの場合も waitpid で子プロセスを片付ける（ゾンビを残さない）。
 * 標準入力は /dev/null。子プロセスではシグナルのマスクを空に戻してから exec する。
 * @param argv コマンドと引数（argv[0] は PATH から探す）
 * @param timeoutMs 待つ上限（ms）
 * @return 結果
 */
CommandResult runCommand(const std::vector<std::string>& argv, int timeoutMs = 2000);

/**
 * ログ用に、コマンドと結果を 1 行にまとめる（例: "wpctl settings ... -> 終了コード 1: エラー文"）。
 * @param argv 実行したコマンド
 * @param result 結果
 * @return 1 行の説明
 */
std::string describeCommand(const std::vector<std::string>& argv, const CommandResult& result);
