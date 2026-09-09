#pragma once
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace gocue::recorder
{
inline void ffCheck(int code, const char* operation)
{
    if (code >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}
struct CodecDelete { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDelete { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDelete { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDelete>;
using FramePtr = std::unique_ptr<AVFrame, FrameDelete>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDelete>;
inline FramePtr ffFrame() { FramePtr p(av_frame_alloc()); if (!p) throw std::bad_alloc(); return p; }
inline PacketPtr ffPacket() { PacketPtr p(av_packet_alloc()); if (!p) throw std::bad_alloc(); return p; }
// Borrowed for the duration of the call. A consumer that queues it must av_packet_ref.
using PacketSink = std::function<void(const AVPacket&)>;
struct Dictionary
{
    AVDictionary* value = nullptr;
    ~Dictionary() { av_dict_free(&value); }
    void set(const char* key, const char* text) { ffCheck(av_dict_set(&value, key, text, 0), "Set FFmpeg option"); }
    Dictionary() = default;
    Dictionary(const Dictionary&) = delete;
    Dictionary& operator=(const Dictionary&) = delete;
};
}
