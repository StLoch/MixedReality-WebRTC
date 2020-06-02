// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

// This is a precompiled header, it must be on its own, followed by a blank
// line, to prevent clang-format from reordering it with other headers.
#include "pch.h"

#include "interop/global_factory.h"
#include "media/local_video_track.h"
#include "peer_connection.h"
#include "rtc_base/refcountedobject.h"

// This attempts to disable audio rendering, allowing higher levels to do things like spatial audio. For now,
// There is a bug on UWP where it doesn't pass audio to the upper layer. Need to investigate, but atm just
// let webrtc do it for us.
#define DISABLE_AUTOMATIC_AUDIO_RENDERING 1

// By default webrtc just crashes if there is any audio device it doesn't support well (RTC_CHECK(adm()); in
// webrtcvoiceengine). For a while we were detecting this ourselves and installing a dummy ADM. For now I've
// modified webrtc to just allow coreaudio even if not everything is supported
// (webrtc\xplatform\webrtc\modules\audio_device\audio_device_impl.cc). One world is we create our own ADM to
// Handle this all more gracefully?
#define INSTALL_DUMMY_ADM_ON_EDGE_CASE 0

namespace {

using namespace Microsoft::MixedReality::WebRTC;

/// Global factory of all global objects, including the peer connection factory
/// itself, with added thread safety. This keeps a track of all objects alive,
/// to determine when it is safe to release the WebRTC threads, thereby allowing
/// a DLL linking this code to be unloaded.
std::unique_ptr<GlobalFactory> g_factory = std::make_unique<GlobalFactory>();

/// Utility to convert an ObjectType to a string, for debugging purpose. This
/// returns a view over a global constant buffer (static storage), which is
/// always valid, never deallocated.
std::string_view ObjectTypeToString(ObjectType type) {
  static_assert((int)ObjectType::kPeerConnection == 0, "");
  static_assert((int)ObjectType::kLocalVideoTrack == 1, "");
  static_assert((int)ObjectType::kExternalVideoTrackSource == 2, "");
  constexpr const std::string_view s_types[] = {
      "PeerConnection", "LocalVideoTrack", "ExternalVideoTrackSource"};
  return s_types[(int)type];
}

/// Utility to format a tracked object into a string, for debugging purpose.
std::string ObjectToString(ObjectType type, TrackedObject* obj) {
  // rtc::StringBuilder doesn't support std::string_view, nor Append(). And
  // asbl::string_view is not constexpr-friendly on MSVC due to strlen().
  // rtc::SimpleStringBuilder supports Append(), but cannot dynamically resize.
  // Assume that the object name will not be too long, and use that one.
  char buffer[512];
  rtc::SimpleStringBuilder builder(buffer);
  builder << "(";
  std::string_view sv = ObjectTypeToString(type);
  builder.Append(sv.data(), sv.length());
  if (obj) {
    builder << ") " << obj->GetName();
  } else {
    builder << ") NULL";
  }
  return builder.str();
}

}  // namespace

namespace Microsoft::MixedReality::WebRTC {

const std::unique_ptr<GlobalFactory>& GlobalFactory::Instance() {
  return g_factory;
}

GlobalFactory::~GlobalFactory() {
  std::scoped_lock lock(mutex_);
  if (!alive_objects_.empty()) {
    // WebRTC object destructors are also dispatched to the signaling thread,
    // like all method calls, but the threads are stopped by the GlobalFactory
    // shutdown, so dispatching will never complete.
    RTC_LOG(LS_ERROR) << "Shutting down the global factory while "
                      << alive_objects_.size()
                      << " objects are still alive. This will likely deadlock.";
    for (auto&& pair : alive_objects_) {
      RTC_LOG(LS_ERROR) << "- " << ObjectToString(pair.second, pair.first)
                        << " [" << pair.first->GetApproxRefCount()
                        << " ref(s)]";
    }
  }
  ShutdownNoLock();
}

rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
GlobalFactory::GetOrCreate() {
  std::scoped_lock lock(mutex_);
  if (!factory_) {
    if (Initialize() != Result::kSuccess) {
      return nullptr;
    }
  }
  return factory_;
}

mrsResult GlobalFactory::GetOrCreate(
    rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>& factory) {
  factory = nullptr;
  std::scoped_lock lock(mutex_);
  if (!factory_) {
    mrsResult res = Initialize();
    if (res != Result::kSuccess) {
      return res;
    }
  }
  factory = factory_;
  return (factory ? Result::kSuccess : Result::kUnknownError);
}

rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
GlobalFactory::GetExisting() noexcept {
  std::scoped_lock lock(mutex_);
  return factory_;
}

rtc::Thread* GlobalFactory::GetWorkerThread() noexcept {
  std::scoped_lock lock(mutex_);
#if defined(WINUWP)
  return impl_->workerThread.get();
#else   // defined(WINUWP)
  return worker_thread_.get();
#endif  // defined(WINUWP)
}

void GlobalFactory::AddObject(ObjectType type, TrackedObject* obj) noexcept {
  try {
    std::scoped_lock lock(mutex_);
    alive_objects_.emplace(obj, type);
  } catch (...) {
  }
}

void GlobalFactory::RemoveObject(ObjectType type, TrackedObject* obj) noexcept {
  try {
    std::scoped_lock lock(mutex_);
    auto it = alive_objects_.find(obj);
    if (it != alive_objects_.end()) {
      RTC_CHECK(it->second == type);
      alive_objects_.erase(it);
      if (alive_objects_.empty()) {
        ShutdownNoLock();
      }
    }
  } catch (...) {
  }
}

#if defined(WINUWP)

using WebRtcFactoryPtr =
    std::shared_ptr<wrapper::impl::org::webRtc::WebRtcFactory>;

WebRtcFactoryPtr GlobalFactory::get() {
  std::scoped_lock lock(mutex_);
  if (!impl_) {
    if (Initialize() != Result::kSuccess) {
      return nullptr;
    }
  }
  return impl_;
}

mrsResult GlobalFactory::GetOrCreateWebRtcFactory(WebRtcFactoryPtr& factory) {
  factory.reset();
  std::scoped_lock lock(mutex_);
  if (!impl_) {
    mrsResult res = Initialize();
    if (res != Result::kSuccess) {
      return res;
    }
  }
  factory = impl_;
  return (factory ? Result::kSuccess : Result::kUnknownError);
}

#endif  // defined(WINUWP)

#if !defined(WINUWP)
#include <algorithm>
#include <vector>
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#define EXIT_ON_ERROR(hres) \
  do {                      \
    if (FAILED(hres)) {     \
        errstr =       \
        std::string("IsDeviceConnected Error: [") + \
                      std::to_string(hres) + "] " + __FILE__ + "@" + \
                      std::to_string(__LINE__) + "\n"; \
        OutputDebugStringA(errstr.c_str()); \
      goto Exit;            \
    }                       \
  } while (false)
#define SAFE_RELEASE(punk) \
  do {                     \
    if ((punk) != NULL) {  \
      (punk)->Release();   \
      (punk) = NULL;       \
    }                      \
  } while (false)
namespace {

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
bool icontains(std::wstring str, std::wstring substr) {
  std::transform(str.begin(), str.end(), str.begin(),
                 [](unsigned char c) { return tolower(c); });
  std::transform(substr.begin(), substr.end(), substr.begin(),
                 [](unsigned char c) { return tolower(c); });
  return (str.find(substr) != std::string::npos);
}
bool IsDeviceConnected(EDataFlow flow, const std::vector<std::wstring>& devices) {
  HRESULT hr = S_OK;
  IMMDeviceEnumerator* pEnumerator = NULL;
  IMMDeviceCollection* pCollection = NULL;
  IMMDevice* pEndpoint = NULL;
  IPropertyStore* pProps = NULL;
  LPWSTR pwszID = NULL;
  bool found = false;
  std::string errstr;
  std::wstring errstrW;
  // Already done.
  // hr = CoInitialize(nullptr);
  // EXIT_ON_ERROR(hr);
  hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                        IID_IMMDeviceEnumerator, (void**)&pEnumerator);
  EXIT_ON_ERROR(hr);
  hr = pEnumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                       &pCollection);
  EXIT_ON_ERROR(hr);
  UINT count;
  hr = pCollection->GetCount(&count);
  EXIT_ON_ERROR(hr);
  if (count == 0) {
    OutputDebugStringA("No endpoints found.\n");
  }
  for (ULONG i = 0; i < count && !found; i++) {
    // Get pointer to endpoint number i.
    hr = pCollection->Item(i, &pEndpoint);
    EXIT_ON_ERROR(hr);
    // Get the endpoint ID string.
    hr = pEndpoint->GetId(&pwszID);
    EXIT_ON_ERROR(hr);
    hr = pEndpoint->OpenPropertyStore(STGM_READ, &pProps);
    EXIT_ON_ERROR(hr);
    PROPVARIANT varName;
    // Initialize container for property value.
    PropVariantInit(&varName);
    // Get the endpoint's friendly-name property.
    hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
    EXIT_ON_ERROR(hr);
    if (varName.pwszVal != nullptr) {
      for (size_t j = 0; j < devices.size() && !found; ++j) {
        if (icontains(varName.pwszVal, devices[j])) {
          errstrW = std::wstring(L"Found matching device: ") + varName.pwszVal +
                    L"\n";
          OutputDebugStringW(errstrW.c_str());
          found = true;
        }
      }
    }
    CoTaskMemFree(pwszID);
    pwszID = NULL;
    PropVariantClear(&varName);
    SAFE_RELEASE(pProps);
    SAFE_RELEASE(pEndpoint);
  }
  SAFE_RELEASE(pEnumerator);
  SAFE_RELEASE(pCollection);
  return found;
Exit:
  OutputDebugStringA("Error testing audio!\n");
  CoTaskMemFree(pwszID);
  SAFE_RELEASE(pEnumerator);
  SAFE_RELEASE(pCollection);
  SAFE_RELEASE(pEndpoint);
  SAFE_RELEASE(pProps);
  return false;
}
}  // namespace

class AudioDeviceModuleThatDoesntCrashOrReallyDoAnything
    : public webrtc::AudioDeviceModule {
 public:
  virtual int32_t ActiveAudioLayer(AudioLayer* audioLayer) const { return 0; }
  virtual int32_t RegisterAudioCallback(
      ::webrtc::AudioTransport* audioCallback) {
    return 0;
  }

  // Main initialization and termination
  virtual int32_t Init() { return 0; }
  virtual int32_t Terminate() { return 0; }
  virtual bool Initialized() const { return true; }

  // Device enumeration
  virtual int16_t PlayoutDevices() { return 0; }
  virtual int16_t RecordingDevices() { return 0; }
  virtual int32_t PlayoutDeviceName(uint16_t index,
                                    char name[::webrtc::kAdmMaxDeviceNameSize],
                                    char guid[::webrtc::kAdmMaxGuidSize]) {
    return 0;
  }
  virtual int32_t RecordingDeviceName(uint16_t index,
      char name[::webrtc::kAdmMaxDeviceNameSize],
      char guid[::webrtc::kAdmMaxGuidSize]) {
    return 0;
  }

  // Device selection
  virtual int32_t SetPlayoutDevice(uint16_t index) { return 0; }
  virtual int32_t SetPlayoutDevice(WindowsDeviceType device) { return 0; }
  virtual int32_t SetRecordingDevice(uint16_t index) { return 0; }
  virtual int32_t SetRecordingDevice(WindowsDeviceType device) { return 0; }

  // Audio transport initialization
  virtual int32_t PlayoutIsAvailable(bool* available) {
    *available = false;
    return 0;
  }
  virtual int32_t InitPlayout() { return 0; }
  virtual bool PlayoutIsInitialized() const {
  return true;
} virtual int32_t RecordingIsAvailable(bool* available) {
  *available = false;
  return 0;
}
virtual int32_t InitRecording() {
  return 0;
}
virtual bool RecordingIsInitialized() const {
  return true;
}

// Audio transport control
virtual int32_t StartPlayout() {
  return 0;
}
virtual int32_t StopPlayout() {
  return 0;
}
virtual bool Playing() const {
  return false;
}
virtual int32_t StartRecording() {
  return 0;
}
virtual int32_t StopRecording() {
  return 0;
}
virtual bool Recording() const {
  return false;
}

// Audio mixer initialization
virtual int32_t InitSpeaker() {
  return 0;
}
virtual bool SpeakerIsInitialized() const {
  return true;
}
virtual int32_t InitMicrophone() {
  return 0;
}
virtual bool MicrophoneIsInitialized() const {
  return true;
}

// Speaker volume controls
virtual int32_t SpeakerVolumeIsAvailable(bool* available) {
  *available = false;
  return 0;
}
virtual int32_t SetSpeakerVolume(uint32_t volume) {
  return 0;
}
virtual int32_t SpeakerVolume(uint32_t* volume) const {
  return 0;
}
virtual int32_t MaxSpeakerVolume(uint32_t* maxVolume) const {
  return 0;
}
virtual int32_t MinSpeakerVolume(uint32_t* minVolume) const {
  return 0;
}

// Microphone volume controls
virtual int32_t MicrophoneVolumeIsAvailable(bool* available) {
  *available = false;
  return 0;
}
virtual int32_t SetMicrophoneVolume(uint32_t volume) {
  return 0;
}
virtual int32_t MicrophoneVolume(uint32_t* volume) const {
  return 0;
}
virtual int32_t MaxMicrophoneVolume(uint32_t* maxVolume) const {
  return 0;
}
virtual int32_t MinMicrophoneVolume(uint32_t* minVolume) const {
  return 0;
}

// Speaker mute control
virtual int32_t SpeakerMuteIsAvailable(bool* available) {
  *available = false;
  return 0;
}
virtual int32_t SetSpeakerMute(bool enable) {
  return 0;
}
virtual int32_t SpeakerMute(bool* enabled) const {
  return 0;
}

// Microphone mute control
virtual int32_t MicrophoneMuteIsAvailable(bool* available) {
  *available = false;
  return 0;
}
virtual int32_t SetMicrophoneMute(bool enable) {
  return 0;
}
virtual int32_t MicrophoneMute(bool* enabled) const {
  return 0;
}

// Stereo support
virtual int32_t StereoPlayoutIsAvailable(bool* available) const {
  *available = false;
  return 0;
}
virtual int32_t SetStereoPlayout(bool enable) {
  return 0;
}
virtual int32_t StereoPlayout(bool* enabled) const {
  return 0;
}
virtual int32_t StereoRecordingIsAvailable(bool* available) const {
  return 0;
}
virtual int32_t SetStereoRecording(bool enable) {
  return 0;
}
virtual int32_t StereoRecording(bool* enabled) const {
  return 0;
}

// Playout delay
virtual int32_t PlayoutDelay(uint16_t* delayMS) const {
  return 0;
}

// Only supported on Android.
virtual bool BuiltInAECIsAvailable() const {
  return false;
}
virtual bool BuiltInAGCIsAvailable() const {
  return false;
}
virtual bool BuiltInNSIsAvailable() const {
  return false;
}

// Enables the built-in audio effects. Only supported on Android.
virtual int32_t EnableBuiltInAEC(bool enable) {
  return 0;
}
virtual int32_t EnableBuiltInAGC(bool enable) {
  return 0;
}
virtual int32_t EnableBuiltInNS(bool enable) {
  return 0;
}
};

#endif

mrsResult GlobalFactory::Initialize() {
  RTC_CHECK(!factory_);

#if defined(WINUWP)
  RTC_CHECK(!impl_);
  auto mw = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView();
  auto cw = mw.CoreWindow();
  auto dispatcher = cw.Dispatcher();
  if (dispatcher.HasThreadAccess()) {
    // WebRtcFactory::setup() will deadlock if called from main UI thread
    // See https://github.com/webrtc-uwp/webrtc-uwp-sdk/issues/143
    return Result::kWrongThread;
  }
  auto dispatcherQueue =
      wrapper::impl::org::webRtc::EventQueue::toWrapper(dispatcher);

  // Setup the WebRTC library
  {
    auto libConfig =
        std::make_shared<wrapper::impl::org::webRtc::WebRtcLibConfiguration>();
    libConfig->thisWeak_ = libConfig;  // mimic wrapper_create()
    libConfig->queue = dispatcherQueue;
    wrapper::impl::org::webRtc::WebRtcLib::setup(libConfig);
  }

  // Create the UWP factory
  {
    auto factoryConfig = std::make_shared<
        wrapper::impl::org::webRtc::WebRtcFactoryConfiguration>();
    factoryConfig->thisWeak_ = factoryConfig;  // mimic wrapper_create()
    factoryConfig->audioCapturingEnabled = true;
#if DISABLE_AUTOMATIC_AUDIO_RENDERING
    factoryConfig->audioRenderingEnabled = false;
#else
    factoryConfig->audioRenderingEnabled = true;
#endif
    factoryConfig->enableAudioBufferEvents = false;
    impl_ = std::make_shared<wrapper::impl::org::webRtc::WebRtcFactory>();
    impl_->thisWeak_ = impl_;  // mimic wrapper_create()
    impl_->wrapper_init_org_webRtc_WebRtcFactory(factoryConfig);
  }
  impl_->internalSetup();

  // Cache the peer connection factory
  factory_ = impl_->peerConnectionFactory();
#else   // defined(WINUWP)
  network_thread_ = rtc::Thread::CreateWithSocketServer();
  RTC_CHECK(network_thread_.get());
  network_thread_->SetName("WebRTC network thread", network_thread_.get());
  network_thread_->Start();
  worker_thread_ = rtc::Thread::Create();
  RTC_CHECK(worker_thread_.get());
  worker_thread_->SetName("WebRTC worker thread", worker_thread_.get());
  worker_thread_->Start();
  signaling_thread_ = rtc::Thread::Create();
  RTC_CHECK(signaling_thread_.get());
  signaling_thread_->SetName("WebRTC signaling thread",
                             signaling_thread_.get());
  signaling_thread_->Start();

#if DISABLE_AUTOMATIC_AUDIO_RENDERING
  static const int16_t zerobuf[200];

  struct PumpSourcesAndDiscardMixer : webrtc::AudioMixer {
    rtc::CriticalSection crit_;
    std::vector<Source*> audio_source_list_;

    bool AddSource(Source* audio_source) override {
      RTC_DCHECK(audio_source);
      rtc::CritScope lock(&crit_);
      RTC_DCHECK(find(audio_source_list_.begin(), audio_source_list_.end(),
                      audio_source) == audio_source_list_.end())
          << "Source already added to mixer";
      RTC_LOG(LS_INFO) << "Adding source to PumpSourcesAndDiscardMixer.";
      audio_source_list_.emplace_back(audio_source);
      return true;
    }
    void RemoveSource(Source* audio_source) override {
      RTC_DCHECK(audio_source);
      rtc::CritScope lock(&crit_);
      const auto iter = find(audio_source_list_.begin(),
                             audio_source_list_.end(), audio_source);
      RTC_DCHECK(iter != audio_source_list_.end())
          << "Source not present in mixer";
      RTC_LOG(LS_INFO) << "Removing source from PumpSourcesAndDiscardMixer.";
      audio_source_list_.erase(iter);
    }
    void Mix(size_t number_of_channels,
             webrtc::AudioFrame* audio_frame_for_mixing) override {
      for (auto& source : audio_source_list_) {
        // This pumps the source and fires the frame observer callbacks
        // which in turn fill the AudioReadStream buffers
        const auto audio_frame_info = source->GetAudioFrameWithInfo(
            source->PreferredSampleRate(), audio_frame_for_mixing);

        if (audio_frame_info == Source::AudioFrameInfo::kError) {
          RTC_LOG_F(LS_WARNING)
              << "failed to GetAudioFrameWithInfo() from source";
          continue;
        }
      }
      // We don't actually want these tracks to add to the mix.
      // So we return an empty frame.
      // TODO: it would be nice for tracks which are connected to a spatial
      // audio source to be intercepted earlier. Currently toggling between
      // local audio rendering and spatial audio is a global switch (not per
      // track nor connection).
      audio_frame_for_mixing->UpdateFrame(
          0, zerobuf, 80, 8000, webrtc::AudioFrame::kNormalSpeech,
          webrtc::AudioFrame::kVadUnknown, number_of_channels);
    }
  };

  webrtc::AudioMixer* mixer = new rtc::RefCountedObject<PumpSourcesAndDiscardMixer>();
#else
  webrtc::AudioMixer* mixer = nullptr;
#endif

#if INSTALL_DUMMY_ADM_ON_EDGE_CASE
  bool disableAudioToPreventNullADM =
      IsDeviceConnected(eCapture, {L"DENON", L"Kinect"}) ||
      IsDeviceConnected(eRender, {L"DENON", L"Kinect"});

  if (disableAudioToPreventNullADM) {
  factory_ = webrtc::CreatePeerConnectionFactory(
      network_thread_.get(), worker_thread_.get(), signaling_thread_.get(),
        new rtc::RefCountedObject<
            AudioDeviceModuleThatDoesntCrashOrReallyDoAnything>(),
        webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      std::unique_ptr<webrtc::VideoEncoderFactory>(
          new webrtc::MultiplexEncoderFactory(
              absl::make_unique<webrtc::InternalEncoderFactory>())),
      std::unique_ptr<webrtc::VideoDecoderFactory>(
          new webrtc::MultiplexDecoderFactory(
              absl::make_unique<webrtc::InternalDecoderFactory>())),
        mixer, nullptr);
  } else {
#else
  {
#endif
    rtc::scoped_refptr<webrtc::AudioDeviceModule> adm_ = nullptr;
    //rtc::scoped_refptr<webrtc::AudioDeviceModule> adm_ =
    //    webrtc::AudioDeviceModule::Create(
    //    webrtc::AudioDeviceModule::kPlatformDefaultAudio);
    //adm_->StartPlayout();

    factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(), worker_thread_.get(), signaling_thread_.get(),
        adm_, webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        std::unique_ptr<webrtc::VideoEncoderFactory>(
            new webrtc::MultiplexEncoderFactory(
                absl::make_unique<webrtc::InternalEncoderFactory>())),
        std::unique_ptr<webrtc::VideoDecoderFactory>(
            new webrtc::MultiplexDecoderFactory(
                absl::make_unique<webrtc::InternalDecoderFactory>())),
        mixer, nullptr);
  }
#endif  // defined(WINUWP)
  return (factory_.get() != nullptr ? Result::kSuccess : Result::kUnknownError);
}

void GlobalFactory::ShutdownNoLock() {
  factory_ = nullptr;
#if defined(WINUWP)
  impl_ = nullptr;
#else   // defined(WINUWP)
  network_thread_.reset();
  worker_thread_.reset();
  signaling_thread_.reset();
#endif  // defined(WINUWP)
}

}  // namespace Microsoft::MixedReality::WebRTC
