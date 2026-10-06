#include <catch2/catch_test_macros.hpp>

#include "core/qwen3_stream.h"
#include "core/win_compat.h"
#include "sensevoice.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// CMake extracts the marked production blocks, so these model-free fixtures
// compile the actual getter/parsing/ownership code without duplicating it.
struct crispasr_session {
    std::string detected_lang;
    std::string source_language;
};
#define CA_EXPORT
#include "session-language-getter.inc"
#undef CA_EXPORT

static int write_detected_language(const std::string& detected, const std::string& source, char* out, int cap) {
    crispasr_session session{detected, source};
    return crispasr_session_detected_language(&session, out, cap);
}

struct QwenMetadata {
    std::string language;
    size_t text_begin;
};

static QwenMetadata qwen_metadata(const std::vector<std::string>& pieces, const std::string& lang, bool raon) {
    crispasr_session session;
    auto* s = &session;
    const bool lang_set = !lang.empty();
#include "session-language-qwen.inc"
    return {s->detected_lang, text_begin};
}

static char* take_sensevoice_text(sensevoice_result* result, std::string& detected) {
    crispasr_session session{detected, ""};
    auto* s = &session;
    char* text = nullptr;
#include "session-language-sensevoice.inc"
    detected = s->detected_lang;
    return text;
}

TEST_CASE("session language: Qwen metadata is separate from transcript tokens", "[unit][session-language]") {
    const std::vector<std::string> pieces = {"<|im_start|>", "language", " Eng",  "lish",
                                             "<asr_text>",   "Hello",    " world"};
    const auto result = qwen_metadata(pieces, "", false);
    CHECK(result.language == "English");
    REQUIRE(result.text_begin == 5);
    CHECK(pieces[result.text_begin] == "Hello");
    CHECK(pieces[result.text_begin + 1] == " world");
}

TEST_CASE("session language: auto enables detection and generated headers win", "[unit][session-language]") {
    const std::vector<std::string> pieces = {"language", " Chinese", "<asr_text>", "你好"};
    CHECK(qwen_metadata(pieces, "auto", false).language == "Chinese");
    CHECK(qwen_metadata(pieces, "en", false).language == "Chinese");
    CHECK(qwen_metadata({"Hello"}, "en", false).language == "en");
    CHECK(qwen_metadata({"Hello"}, "auto", false).language.empty());
}

TEST_CASE("session language: multi-token names and whitespace are consumed completely", "[unit][session-language]") {
    const auto result =
        qwen_metadata({"[PAD0]", "language", " \t", "Can", "ton", "ese", " \n", "<asr_text>", "你好"}, "", false);
    CHECK(result.language == "Cantonese");
    CHECK(result.text_begin == 8);
}

TEST_CASE("session language: plain answers and Raon text are not language metadata", "[unit][session-language]") {
    const std::vector<std::string> plain = {"The", " answer", " is", " English"};
    CHECK(qwen_metadata(plain, "", false).language.empty());
    CHECK(qwen_metadata(plain, "", false).text_begin == 0);
    const auto raon = qwen_metadata({"language", " Korean", "<asr_text>", "안녕"}, "en", true);
    CHECK(raon.language.empty());
    CHECK(raon.text_begin == 0);
    CHECK(qwen_metadata({"language", " English"}, "", false).language.empty());
    CHECK(qwen_metadata({"language", " English"}, "", false).text_begin == 0);
}

TEST_CASE("session language: missing metadata does not retain a previous detection", "[unit][session-language]") {
    std::string cached = qwen_metadata({"language", " Japanese", "<asr_text>", "こんにちは"}, "", false).language;
    REQUIRE(cached == "Japanese");
    cached = qwen_metadata({"language", " none", "<asr_text>", "<non_speech>"}, "", false).language;
    CHECK(cached.empty());
    cached = qwen_metadata({"Hello"}, "", false).language;
    CHECK(cached.empty());
}

TEST_CASE("session language: buffer contract preserves full length and bounds", "[unit][session-language]") {
    char buf[16] = {};
    CHECK(crispasr_session_detected_language(nullptr, buf, sizeof(buf)) == -1);
    CHECK(write_detected_language("English", "zh", buf, sizeof(buf)) == 7);
    CHECK(std::string(buf) == "English");
    CHECK(write_detected_language("", "zh", buf, sizeof(buf)) == 2);
    CHECK(std::string(buf) == "zh");
    CHECK(write_detected_language("", "", buf, sizeof(buf)) == 7);
    CHECK(std::string(buf) == "unknown");
    CHECK(write_detected_language("", "auto", buf, sizeof(buf)) == 7);
    CHECK(std::string(buf) == "unknown");
    std::memset(buf, '#', sizeof(buf));
    CHECK(write_detected_language("English", "", buf, 4) == 7);
    CHECK(std::string(buf) == "Eng");
    CHECK(buf[4] == '#');
    CHECK(write_detected_language("English", "", buf, 1) == 7);
    CHECK(buf[0] == '\0');
    CHECK(buf[1] == 'n');
    CHECK(write_detected_language("en", "", nullptr, 8) == -1);
    buf[0] = '#';
    CHECK(write_detected_language("en", "", buf, 0) == -1);
    CHECK(write_detected_language("en", "", buf, -1) == -1);
    CHECK(buf[0] == '#');
}

TEST_CASE("session language: SenseVoice copies language and transfers text ownership", "[unit][session-language]") {
    sensevoice_result result = {};
    result.language = strdup("yue");
    result.text = strdup("你好");
    REQUIRE(result.language != nullptr);
    REQUIRE(result.text != nullptr);
    char* original_text = result.text;
    std::string cached = "en";
    char* text = take_sensevoice_text(&result, cached);
    CHECK(text == original_text);
    CHECK(result.text == nullptr);
    CHECK(cached == "yue");
    std::free(result.language);
    std::free(result.text); // Same ownership rule as sensevoice_result_free.
    CHECK(cached == "yue");
    CHECK(std::string(text) == "你好");
    std::free(text);
}

TEST_CASE("session language: SenseVoice failure clears stale detection", "[unit][session-language]") {
    std::string cached = "zh";
    CHECK(take_sensevoice_text(nullptr, cached) == nullptr);
    CHECK(cached.empty());
    sensevoice_result result = {};
    cached = "en";
    CHECK(take_sensevoice_text(&result, cached) == nullptr);
    CHECK(cached.empty());
}
