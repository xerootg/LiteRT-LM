// JNI for Kokoro Reader: drives LiteRT-LM's Kokoro text-to-speech directly, with a configurable
// text chunk size. The MediaPipe task wraps the same engine but pins the chunk at ~100 characters;
// with a model exported for 512 phoneme tokens, a whole sentence or paragraph can be one run.
//
// Kotlin side: com.brahmadeo.supertonic.tts.KokoroNative (see the app).
#include <jni.h>

#include <algorithm>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "omni/base/io_types.h"
#include "omni/omni_engine.h"
#include "omni/omni_session.h"
#include "runtime/proto/text2speech_metadata.pb.h"
#include "runtime/proto/text2speech_session_config.pb.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "KokoroNative", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "KokoroNative", __VA_ARGS__)
#else
#include <cstdio>
#define LOGI(...) (fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n"))
#define LOGE(...) (fprintf(stderr, __VA_ARGS__), fprintf(stderr, "\n"))
#endif

namespace {

struct Handle {
  std::unique_ptr<litert::omni::OmniEngine> engine;
  std::string last_error;
  int sample_rate_hz = 24000;
};

std::string JString(JNIEnv* env, jstring s) {
  if (s == nullptr) return "";
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out(c ? c : "");
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

}  // namespace

extern "C" {

// Returns a handle (0 on failure; nativeLastError on a 0 handle is not possible, so the error is
// logged). backend: 0 = CPU, 1 = GPU (vocoder on GPU; the acoustic stage stays on CPU, as in the
// stock runtime). maxChunkChars: 0 keeps the runtime default (about 100), larger values need a model
// with a matching phoneme bucket. sentenceDelimitersOnly: split only at . ? ! and newline.
JNIEXPORT jlong JNICALL Java_com_brahmadeo_supertonic_tts_KokoroNative_nativeCreate(
    JNIEnv* env, jclass, jstring model_path, jint backend, jstring cache_dir, jstring language,
    jstring voice, jint max_chunk_chars, jboolean sentence_delimiters_only, jint num_threads) {
  litert::omni::OmniEngine::Settings settings;
  settings.model_path = JString(env, model_path);
  settings.cache_dir = JString(env, cache_dir);
  settings.num_threads = num_threads > 0 ? num_threads : 4;
  settings.backend = backend == 1 ? litert::omni::OmniEngine::Settings::Backend::kGpu
                                  : litert::omni::OmniEngine::Settings::Backend::kCpu;

  litert::lm::proto::Text2SpeechSessionConfig session_config;
  session_config.set_language(JString(env, language));
  session_config.set_voice(JString(env, voice));
  if (max_chunk_chars > 0) {
    session_config.mutable_text_chunk_config()->set_max_buffer_size(max_chunk_chars);
  }
  if (sentence_delimiters_only) {
    auto* chunk = session_config.mutable_text_chunk_config();
    chunk->clear_delimiters();
    for (const char* d : {".", "?", "!", "\n"}) chunk->add_delimiters(d);
  }

  litert::lm::proto::Text2SpeechMetadata metadata;
  auto* kokoro = metadata.mutable_tts_model_type()->mutable_kokoro();
  kokoro->set_voice_name(JString(env, voice));
  kokoro->set_language(JString(env, language));
  kokoro->set_target_bucket(0);
  if (backend == 1) kokoro->set_vocoder_backend("gpu");

  auto engine = litert::omni::OmniEngine::Create("kokoro", settings, &session_config, &metadata);
  if (!engine.ok()) {
    LOGE("Kokoro engine failed: %s", engine.status().ToString().c_str());
    return 0;
  }
  auto* h = new Handle();
  h->engine = std::move(*engine);
  LOGI("Kokoro engine ready (backend %d, chunk %d chars)", backend, max_chunk_chars);
  return reinterpret_cast<jlong>(h);
}

// Synthesises one text (any length: the runtime chunks it at sentence ends and at maxChunkChars) and
// returns all the audio as float samples at nativeSampleRate(). Returns null on failure.
JNIEXPORT jfloatArray JNICALL Java_com_brahmadeo_supertonic_tts_KokoroNative_nativeSynthesize(
    JNIEnv* env, jclass, jlong handle, jstring text) {
  auto* h = reinterpret_cast<Handle*>(handle);
  if (h == nullptr || !h->engine) return nullptr;
  auto input_source = std::make_unique<litert::omni::PushInputSource>();
  auto* raw = input_source.get();
  auto session = h->engine->CreateSession(std::move(input_source));
  if (!session.ok()) {
    h->last_error = session.status().ToString();
    LOGE("session: %s", h->last_error.c_str());
    return nullptr;
  }
  auto st = raw->PushInput(litert::omni::OmniSession::TextInput{.text = JString(env, text)});
  if (!st.ok()) {
    h->last_error = st.ToString();
    return nullptr;
  }
  raw->Finish();

  std::vector<float> pcm;
  auto take = [&](const litert::omni::Output& out) {
    if (const auto* a = std::get_if<litert::omni::AudioOutput>(&out)) {
      pcm.insert(pcm.end(), a->pcm_samples.begin(), a->pcm_samples.end());
      if (a->sample_rate_hz > 0) h->sample_rate_hz = a->sample_rate_hz;
    }
  };
  while (true) {
    auto out = (*session)->ProcessNext();
    if (out.ok()) { take(*out); continue; }
    if (absl::IsOutOfRange(out.status()) || absl::IsNotFound(out.status())) break;
    h->last_error = out.status().ToString();
    LOGE("ProcessNext: %s", h->last_error.c_str());
    break;
  }
  auto flushed = (*session)->Flush();
  if (flushed.ok()) take(*flushed);

  jfloatArray arr = env->NewFloatArray(static_cast<jsize>(pcm.size()));
  if (arr == nullptr) return nullptr;
  env->SetFloatArrayRegion(arr, 0, static_cast<jsize>(pcm.size()), pcm.data());
  return arr;
}

JNIEXPORT jint JNICALL Java_com_brahmadeo_supertonic_tts_KokoroNative_nativeSampleRate(
    JNIEnv*, jclass, jlong handle) {
  auto* h = reinterpret_cast<Handle*>(handle);
  return h ? h->sample_rate_hz : 24000;
}

JNIEXPORT jstring JNICALL Java_com_brahmadeo_supertonic_tts_KokoroNative_nativeLastError(
    JNIEnv* env, jclass, jlong handle) {
  auto* h = reinterpret_cast<Handle*>(handle);
  return env->NewStringUTF(h ? h->last_error.c_str() : "");
}

JNIEXPORT void JNICALL Java_com_brahmadeo_supertonic_tts_KokoroNative_nativeDestroy(
    JNIEnv*, jclass, jlong handle) {
  delete reinterpret_cast<Handle*>(handle);
}

}  // extern "C"
