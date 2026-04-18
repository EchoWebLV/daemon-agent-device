# =============================================================================
#  PlatformIO pre-build hook: injects connecttoElevenlabs() into the locally
#  installed ESP32-audioI2S library (tag 2.0.6) so Daemon can stream
#  ElevenLabs TTS MP3 directly into the decoder instead of writing it to
#  flash first.
#
#  The library lives under .pio/libdeps/ which is gitignored, so we need a
#  reproducible way to apply our patch on every clean build. This script:
#
#    1. Finds the installed copy of Audio.cpp / Audio.h
#    2. If our marker "connecttoElevenlabs" is already present, does nothing
#    3. Otherwise inserts the method declaration + implementation in place
#
#  Registered via `extra_scripts` in platformio.ini.
# =============================================================================
Import("env")   # noqa: F821

import os
import re
import sys

MARKER = "connecttoElevenlabs"

HEADER_INSERT_AFTER = (
    "bool connecttomarytts(const char* speech, const char* lang, const char* voice);"
)
HEADER_INSERT = """
    // ---- Daemon patch (non-upstream) -----------------------------------
    // Streams an ElevenLabs TTS MP3 directly into the decoder (HTTPS POST).
    // Starts playback as soon as the first ~4 KB of audio arrives instead
    // of waiting for the entire file to download to flash.
    bool connecttoElevenlabs(const char* voiceId, const char* apiKey,
                             const char* text,    const char* modelId);
    // --------------------------------------------------------------------
"""

CPP_INSERT_AFTER = "bool Audio::connecttomarytts(const char* speech, const char* lang, const char* voice){"

CPP_METHOD = r"""
// ---- Daemon patch (non-upstream) ---------------------------------------
// HTTPS POST to ElevenLabs `/v1/text-to-speech/{voice_id}/stream`, pipe the
// MP3 response body directly into the existing audio pipeline. The library
// then parses the response headers and starts decoding as soon as ~4 KB of
// MP3 arrives, shaving ~1 s off the naive "download fully, then play" path.
bool Audio::connecttoElevenlabs(const char* voiceId, const char* apiKey,
                                const char* text,    const char* modelId) {
    if(voiceId == NULL || apiKey == NULL || text == NULL) return false;

    AUDIO_INFO("ElevenLabs TTS stream (%u chars)", (unsigned)strlen(text));
    setDefaults();
    m_f_ssl = true;
    m_f_tts = true;

    const char *host = "api.elevenlabs.io";
    const uint16_t port = 443;
    if(!modelId || !*modelId) modelId = "eleven_turbo_v2_5";

    char path[160];
    snprintf(path, sizeof(path),
        "/v1/text-to-speech/%s/stream?output_format=mp3_44100_128", voiceId);

    size_t textLen = strlen(text);
    size_t bodyCap = textLen * 2 + 320;
    char *body = (char*)malloc(bodyCap);
    if(!body) { log_e("oom body"); return false; }
    char *bp = body;
    bp += sprintf(bp, "{\"text\":\"");
    for(size_t i = 0; i < textLen; ++i) {
        uint8_t c = (uint8_t)text[i];
        if(c == '"')       { *bp++='\\'; *bp++='"'; }
        else if(c == '\\') { *bp++='\\'; *bp++='\\'; }
        else if(c == '\n') { *bp++='\\'; *bp++='n'; }
        else if(c == '\r') { *bp++='\\'; *bp++='r'; }
        else if(c >= 0x20) { *bp++ = (char)c; }
    }
    bp += sprintf(bp,
        "\",\"model_id\":\"%s\","
        "\"voice_settings\":{\"stability\":0.45,\"similarity_boost\":0.8,"
        "\"style\":0.3,\"use_speaker_boost\":true}}",
        modelId);
    size_t bodyLen = (size_t)(bp - body);

    char rqh[512];
    int rqhLen = snprintf(rqh, sizeof(rqh),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "xi-api-key: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Accept: audio/mpeg\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, apiKey, (unsigned)bodyLen);

    clientsecure.setInsecure();
    _client = static_cast<WiFiClient*>(&clientsecure);

    uint32_t t = millis();
    if(!_client->connect(host, port, m_timeout_ms_ssl)) {
        AUDIO_INFO("ElevenLabs connect failed");
        free(body);
        return false;
    }
    AUDIO_INFO("SSL to ElevenLabs in %u ms, free heap %u",
               (unsigned)(millis() - t), (unsigned)ESP.getFreeHeap());

    size_t totalLen = (size_t)rqhLen + bodyLen;
    char *both = (char*)malloc(totalLen);
    if(!both) { free(body); AUDIO_INFO("oom writing"); return false; }
    memcpy(both,           rqh,  rqhLen);
    memcpy(both + rqhLen,  body, bodyLen);
    size_t wrote = _client->write((const uint8_t*)both, totalLen);
    free(body);
    free(both);
    AUDIO_INFO("ElevenLabs POST: headers=%d body=%u wrote=%u",
               rqhLen, (unsigned)bodyLen, (unsigned)wrote);
    if(wrote != totalLen) AUDIO_INFO("WARN: short write on POST");

    strncpy(m_lastHost, host, sizeof(m_lastHost) - 1);
    m_f_running     = true;
    m_expectedCodec = CODEC_MP3;
    setDatamode(HTTP_RESPONSE_HEADER);
    m_streamType    = ST_WEBFILE;

    return true;
}
// ---- end Daemon patch --------------------------------------------------
"""


def _find_lib_src(project_dir):
    for root, _, files in os.walk(os.path.join(project_dir, ".pio", "libdeps")):
        if "Audio.cpp" in files and "Audio.h" in files and "ESP32-audioI2S" in root:
            return root
    return None


def apply_patch(*args, **kwargs):
    project_dir = env["PROJECT_DIR"]          # noqa: F821
    libroot = _find_lib_src(project_dir)
    if libroot is None:
        print("[daemon-patch] ESP32-audioI2S not yet installed; will patch after pio installs it", file=sys.stderr)
        return

    header_path = os.path.join(libroot, "Audio.h")
    cpp_path    = os.path.join(libroot, "Audio.cpp")

    with open(header_path, "r", encoding="utf-8") as f:
        header = f.read()
    with open(cpp_path, "r", encoding="utf-8") as f:
        cpp = f.read()

    if MARKER in header and MARKER in cpp:
        return                                    # already patched

    # Header
    if MARKER not in header:
        if HEADER_INSERT_AFTER not in header:
            print("[daemon-patch] ERROR: header anchor missing in Audio.h", file=sys.stderr)
            return
        header = header.replace(
            HEADER_INSERT_AFTER,
            HEADER_INSERT_AFTER + HEADER_INSERT,
            1,
        )
        with open(header_path, "w", encoding="utf-8") as f:
            f.write(header)
        print("[daemon-patch] inserted declaration in Audio.h")

    # Implementation — insert the entire block BEFORE connecttomarytts's definition.
    if MARKER not in cpp:
        if CPP_INSERT_AFTER not in cpp:
            print("[daemon-patch] ERROR: cpp anchor missing in Audio.cpp", file=sys.stderr)
            return
        cpp = cpp.replace(
            CPP_INSERT_AFTER,
            CPP_METHOD + CPP_INSERT_AFTER,
            1,
        )
        with open(cpp_path, "w", encoding="utf-8") as f:
            f.write(cpp)
        print("[daemon-patch] inserted connecttoElevenlabs() in Audio.cpp")


# Run immediately (before compilation). The library source is already
# installed by the time `extra_scripts` runs because platformio resolves
# dependencies before loading scripts.
apply_patch()
