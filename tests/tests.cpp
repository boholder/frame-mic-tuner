// 回帰テスト: cairo・PipeWire・Vulkan・OpenVR を使わない純粋なロジックだけを試す。
// 画面や実機が要らない部分（JSON・設定・文言・出力の読み取り・ワーカーの待ち行列・色のコントラスト比）を
// 決まった入力で確かめる。ビルド: cmake -DBUILD_TESTING=ON ... && ctest。
#include "command.h"
#include "config.h"
#include "i18n.h"
#include "json.h"
#include "mic_state.h"
#include "mic_worker.h"
#include "theme.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int gChecks = 0;
int gFailures = 0;

/** 1 つの確かめを記録する。 */
void record(const char* file, int line, const char* expression, bool ok) {
    ++gChecks;
    if (ok) return;
    ++gFailures;
    std::printf("不合格 %s:%d  %s\n", file, line, expression);
}

/** 条件が成り立つことを確かめる。 */
#define CHECK(cond) record(__FILE__, __LINE__, #cond, (cond))

// ---- JSON ----

void testJson() {
    JsonValue root;
    std::string error;

    CHECK(parseJson(R"({"a": 1, "b": true, "c": null, "d": "x"})", root, error));
    CHECK(root.isObject());
    CHECK(root.get("a") != nullptr && root.get("a")->isNumber() && root.get("a")->number == 1.0);
    CHECK(root.get("b") != nullptr && root.get("b")->isBool() && root.get("b")->boolean);
    CHECK(root.get("c") != nullptr && root.get("c")->type == JsonValue::Type::Null);
    CHECK(root.get("d") != nullptr && root.get("d")->isString() && root.get("d")->text == "x");
    CHECK(root.get("missing") == nullptr);

    // 入れ子の配列・オブジェクト
    CHECK(parseJson(R"([1, [2, 3], {"k": [true, false]}])", root, error));
    CHECK(root.type == JsonValue::Type::Array && root.items.size() == 3);
    CHECK(root.items[0].number == 1.0);
    CHECK(root.items[1].items.size() == 2 && root.items[1].items[1].number == 3.0);
    CHECK(root.items[2].get("k") != nullptr && root.items[2].get("k")->items[1].boolean == false);

    // 数値の形
    CHECK(parseJson("-12.5e2", root, error) && root.number == -1250.0);
    CHECK(parseJson("0.25", root, error) && root.number == 0.25);

    // 文字列のエスケープ（\n・\"・\\・\uXXXX・サロゲートペア）
    CHECK(parseJson(R"("a\nb\t\"c\"\\d")", root, error));
    CHECK(root.text == "a\nb\t\"c\"\\d");
    CHECK(parseJson(R"("\u3042")", root, error));          // あ
    CHECK(root.text == "\xE3\x81\x82");
    CHECK(parseJson(R"("\uD83D\uDE00")", root, error));    // 😀
    CHECK(root.text == "\xF0\x9F\x98\x80");

    // 空の配列・オブジェクト
    CHECK(parseJson("[]", root, error) && root.items.empty());
    CHECK(parseJson("{}", root, error) && root.members.empty());

    // 誤り
    CHECK(!parseJson("1 2", root, error));
    CHECK(!parseJson("\"unclosed", root, error));
    CHECK(!parseJson("[1, 2", root, error));
    CHECK(!parseJson("{\"a\" 1}", root, error));
    CHECK(!parseJson("{a: 1}", root, error));
    CHECK(!parseJson("", root, error));

    // オブジェクトでない値への get
    CHECK(parseJson("42", root, error) && root.get("a") == nullptr);
}

// ---- 文言（i18n） ----

void testI18n() {
    Language language = Language::En;
    CHECK(parseLanguage("ja", language) && language == Language::Ja);
    CHECK(parseLanguage("en", language) && language == Language::En);
    CHECK(parseLanguage("sc", language) && language == Language::Sc);
    CHECK(!parseLanguage("zh", language));
    CHECK(!parseLanguage("", language));

    CHECK(std::strcmp(languageCode(Language::Ja), "ja") == 0);
    CHECK(std::strcmp(languageCode(Language::En), "en") == 0);
    CHECK(std::strcmp(languageCode(Language::Sc), "sc") == 0);

    CHECK(uiText(Language::Ja).title != nullptr);
    CHECK(&uiText(Language::Ja) != &uiText(Language::En));
    CHECK(&uiText(Language::En) == &uiText(Language::En));
    CHECK(uiText(Language::Ja).on != nullptr && uiText(Language::Sc).on != nullptr);
}

// ---- マイクの状態の読み取り ----

void testMicrophoneState() {
    bool value = false;
    CHECK(parseSettingValue("Value: true (Saved: true)\n", value) && value);
    CHECK(parseSettingValue("Value: false\t[Saved: false]\n", value) && !value);
    CHECK(!parseSettingValue("Setting 'frame-mic.echo-cancel' not found\n", value));

    const std::string list =
        "Settings:\n\n- Id: frame-mic.echo-cancel\n  Value: false\t[Saved: false]\n\n"
        "- Id: frame-mic.noise-suppression\n  Default: false\n  Value: true\t[Saved: true]\n";
    bool echo = true;
    bool ns = false;
    CHECK(parseSettingFromList(list, kEchoCancelKey, echo) && !echo);
    CHECK(parseSettingFromList(list, kNoiseSuppressionKey, ns) && ns);
    CHECK(!parseSettingFromList(list, "frame-mic.other", ns));

    // 未使用: マイクから既定のマイク（loopback）へ直接つながる
    const std::string idle =
        "alsa_input.platform-sound.HiFi__Mic__source:capture_MONO\n"
        "  |-> alsa_loopback_stream.alsa_input.platform-sound.HiFi__Mic__source:input_MONO\n";
    MicState state;
    parseLinks(idle, state);
    CHECK(state.linksKnown && !state.inUse && state.chain.empty());

    // 使用中: EQ だけが入っている
    const std::string eqOnly =
        "alsa_input.platform-sound.HiFi__Mic__source:capture_FL\n"
        "  |-> eq_capture:input_FL\n"
        "eq_source:capture_MONO\n"
        "  |-> alsa_loopback_stream.alsa_input.platform-sound.HiFi__Mic__source:input_MONO\n";
    parseLinks(eqOnly, state);
    CHECK(state.linksKnown && state.inUse && state.chain.size() == 1);
    CHECK(state.chain[0].kind == ChainStage::Kind::Eq && state.chain[0].name == "eq");

    // 使用中: EQ → エコー除去 → ノイズ除去。録音しているアプリも拾う
    const std::string full =
        "alsa_input.platform-sound.HiFi__Mic__source:capture_FL\n"
        "  |-> eq_capture:input_FL\n"
        "eq_source:capture_MONO\n"
        "  |-> echo_cancel_capture:input_MONO\n"
        "echo_cancel_source:capture_MONO\n"
        "  |-> ns_capture:input_MONO\n"
        "ns_source:capture_MONO\n"
        "  |-> alsa_loopback_stream.alsa_input.platform-sound.HiFi__Mic__source:input_MONO\n"
        "alsa_loopback_device.alsa_input.platform-sound.HiFi__Mic__source:capture_MONO\n"
        "  |-> some_app:input_MONO\n";
    parseLinks(full, state);
    CHECK(state.linksKnown && state.inUse && state.chain.size() == 3);
    CHECK(state.chain[0].kind == ChainStage::Kind::Eq);
    CHECK(state.chain[1].kind == ChainStage::Kind::EchoCancel);
    CHECK(state.chain[2].kind == ChainStage::Kind::NoiseSuppression);
    CHECK(state.users.size() == 1 && state.users[0] == "some_app");

    // つながりをたどれない（loopback に着かない）ときは chain を出さない
    parseLinks("unrelated:a\n  |-> other:b\n", state);
    CHECK(!state.linksKnown && !state.inUse && state.chain.empty());

    CHECK(parseAutostart("not-found\n") == Autostart::Missing);
    CHECK(parseAutostart("disabled\n") == Autostart::Disabled);
    CHECK(parseAutostart("enabled\n") == Autostart::Enabled);
    CHECK(parseAutostart("enabled-runtime\n") == Autostart::Enabled);
    CHECK(parseAutostart("masked\n") == Autostart::Unknown);

    const std::string dump = R"json([{"id": 53, "type": "PipeWire:Interface:Node", "info": {
        "props": {"node.name": "ns_capture"},
        "params": {"Props": [{"volume": 1.0},
            {"params": ["noise_suppressor_mono:VAD Threshold (%)", 23.0,
                        "noise_suppressor_mono:VAD Grace Period (ms)", 500.0]}]}}}])json";
    const NsParams params = parseNsDump(dump);
    CHECK(params.nodeKnown && params.nodeId == 53);
    CHECK(params.vadKnown && params.vad == 23.0 && params.graceKnown && params.grace == 500.0);
    CHECK(!parseNsDump("[]").nodeKnown);
    CHECK(!parseNsDump("not json").nodeKnown);

    CHECK(clampNsVad(150) == 99.0);
    CHECK(clampNsVad(-1) == 0.0);
    CHECK(clampNsVad(23.4) == 23.0);
    CHECK(clampNsGrace(-5) == 0.0);
    CHECK(clampNsGrace(512) == 510.0);
    CHECK(clampNsGrace(1001) == 1000.0);

    MicState a;
    MicState b;
    CHECK(sameMicState(a, b));
    a.inUse = true;
    CHECK(!sameMicState(a, b));
    b.inUse = true;
    CHECK(sameMicState(a, b));
    a.nsParams.nodeKnown = true;
    a.nsParams.nodeId = 53;
    CHECK(!sameMicState(a, b));

    // つながりの表示は日本語で 1 行
    MicState chainState;
    parseLinks(full, chainState);
    CHECK(describeChain(chainState) == "マイク → EQ → エコー除去 → ノイズ除去 → 出力");
    CHECK(describeChain(MicState()) == "（つながりを読めません）");
}

// ---- 設定ファイル ----

void testConfig() {
    // ファイルが無ければ既定値で成功
    Config defaults;
    std::vector<std::string> warnings;
    std::string error;
    CHECK(loadConfig("/nonexistent/frame-mic-tuner/config.json", defaults, warnings, error));
    CHECK(defaults.tab == PanelTab::Quick && !defaults.hasNsParams);

    // 一時ディレクトリで読み書き
    char dirTemplate[] = "/tmp/frame-mic-tuner-test-XXXXXX";
    char* dir = ::mkdtemp(dirTemplate);
    CHECK(dir != nullptr);
    const std::string path = std::string(dir) + "/sub/config.json";

    Config saved;
    saved.language = Language::En;
    saved.tab = PanelTab::Fine;
    saved.hasNsParams = true;
    saved.nsVad = 30;
    saved.nsGrace = 600;
    saved.updateCheck = false;
    CHECK(saveConfig(path, saved, error));  // 親フォルダも作る

    Config loaded;
    warnings.clear();
    CHECK(loadConfig(path, loaded, warnings, error));
    CHECK(warnings.empty());
    CHECK(loaded.language == Language::En && loaded.tab == PanelTab::Fine);
    CHECK(loaded.hasNsParams && loaded.nsVad == 30 && loaded.nsGrace == 600);
    CHECK(!loaded.updateCheck);

    // 知らないキーは警告になるだけ
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs(R"({"language":"sc","tab":"fine","unknown_key":1})", f);
        std::fclose(f);
        warnings.clear();
        Config config;
        CHECK(loadConfig(path, config, warnings, error));
        CHECK(config.language == Language::Sc && config.tab == PanelTab::Fine);
        CHECK(warnings.size() == 1);
    }

    // 種類が違う値・片方だけのノイズ除去の強さは警告して既定のまま
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs(R"({"language":123,"tab":"weird","ns_vad_threshold_percent":40})", f);
        std::fclose(f);
        warnings.clear();
        Config config;
        CHECK(loadConfig(path, config, warnings, error));
        CHECK(warnings.size() == 3);
        CHECK(config.language == systemLanguage() && config.tab == PanelTab::Quick && !config.hasNsParams);
    }

    ::unlink(path.c_str());
    ::rmdir((std::string(dir) + "/sub").c_str());
    ::rmdir(dir);
}

// ---- 色のコントラスト比 ----

void testTheme() {
    const Color white {1.0, 1.0, 1.0};
    const Color black {0.0, 0.0, 0.0};
    CHECK(contrastRatio(white, black) > 20.9 && contrastRatio(white, black) < 21.1);
    CHECK(contrastRatio(white, white) == 1.0);
    CHECK(contrastRatio(black, white) == contrastRatio(white, black));
    CHECK(requiredRatio(ContrastKind::Text) == 4.5);
    CHECK(requiredRatio(ContrastKind::Ui) == 3.0);
    CHECK(requiredRatio(ContrastKind::Disabled) == 3.0);

    // --contrast-report と同じ組み合わせがすべて合格すること（回帰の錠）
    for (const ContrastPair& pair : contrastPairs()) {
        record(__FILE__, __LINE__, pair.what, contrastRatio(pair.fg, pair.bg) >= requiredRatio(pair.kind));
    }
}

// ---- 外部コマンド ----

void testCommand() {
    CommandResult result = runCommand({"true"});
    CHECK(result.ok() && result.exitCode == 0);

    result = runCommand({"false"});
    CHECK(!result.ok() && result.status == CommandResult::Status::Failed && result.exitCode == 1);

    result = runCommand({"echo", "hello"});
    CHECK(result.ok() && result.out == "hello\n");

    result = runCommand({"this-command-does-not-exist-12345"});
    CHECK(result.status == CommandResult::Status::SpawnFailed);

    result = runCommand({"sleep", "5"}, 100);
    CHECK(result.status == CommandResult::Status::Timeout);

    CHECK(!describeCommand({"echo", "hi"}, result).empty());
}

// ---- ワーカーの待ち行列（スレッドを始めずに） ----

void testMicWorkerQueue() {
    MicWorker worker;
    MicState state;
    CHECK(worker.snapshot(state) == 0 && !state.loaded);
    CHECK(worker.version() == 0);

    // ノイズ除去の強さは、まだ実行していない前の値を捨てて最後の 1 つだけ残す（捨てた分は終わった扱い）
    worker.request({MicCommand::Kind::SetNsParams, false, 10.0, 100.0});
    worker.request({MicCommand::Kind::SetNsParams, false, 20.0, 200.0});
    worker.request({MicCommand::Kind::SetNsParams, false, 30.0, 300.0});
    CHECK(worker.completed() == 2);  // 先の 2 つは捨てられた

    // ほかのコマンドは捨てない
    const uint64_t ticket = worker.request({MicCommand::Kind::SetEcho, true});
    CHECK(ticket == 4);
    CHECK(worker.completed() == 2);
}

}  // namespace

int main() {
    testJson();
    testI18n();
    testMicrophoneState();
    testConfig();
    testTheme();
    testCommand();
    testMicWorkerQueue();

    std::printf("%d 件中 %d 件が不合格\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
