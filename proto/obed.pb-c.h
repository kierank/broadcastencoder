/* Generated by the protocol buffer compiler.  DO NOT EDIT! */

#ifndef PROTOBUF_C_obed_2eproto__INCLUDED
#define PROTOBUF_C_obed_2eproto__INCLUDED

#include <google/protobuf-c/protobuf-c.h>

PROTOBUF_C_BEGIN_DECLS


typedef struct _Obed__InputOpts Obed__InputOpts;
typedef struct _Obed__VideoOpts Obed__VideoOpts;
typedef struct _Obed__AudioOpts Obed__AudioOpts;
typedef struct _Obed__AncillaryOpts Obed__AncillaryOpts;
typedef struct _Obed__MuxOpts Obed__MuxOpts;
typedef struct _Obed__OutputOpts Obed__OutputOpts;
typedef struct _Obed__EncoderControl Obed__EncoderControl;
typedef struct _Obed__EncoderResponse Obed__EncoderResponse;


/* --- enums --- */


/* --- messages --- */

struct  _Obed__InputOpts
{
  ProtobufCMessage base;
  int32_t input_device;
  int32_t card_idx;
  int32_t video_format;
};
#define OBED__INPUT_OPTS__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&obed__input_opts__descriptor) \
    , 0, 0, 0 }


struct  _Obed__VideoOpts
{
  ProtobufCMessage base;
  int32_t profile;
  int32_t bitrate;
  int32_t vbv_bufsize;
  int32_t keyint;
  int32_t bframes;
  int32_t max_refs;
  int32_t lookahead;
  int32_t quality_metric;
  int32_t pid;
  int32_t width;
  int32_t aspect_ratio;
  int32_t afd_passthrough;
  int32_t wss_to_afd;
  int32_t frame_packing;
};
#define OBED__VIDEO_OPTS__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&obed__video_opts__descriptor) \
    , 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }


struct  _Obed__AudioOpts
{
  ProtobufCMessage base;
  int32_t format;
  int32_t channel_map;
  int32_t bitrate;
  int32_t sdi_pair;
  int32_t aac_encap;
  int32_t mp2_mode;
  int32_t mono_channel;
  int32_t reference_level;
  int32_t pid;
  char *lang_code;
  int32_t type;
};
#define OBED__AUDIO_OPTS__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&obed__audio_opts__descriptor) \
    , 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, 0 }


struct  _Obed__AncillaryOpts
{
  ProtobufCMessage base;
  int32_t cea_608;
  int32_t cea_708;
  int32_t ttx_type;
  int32_t ttx_lang_code;
  int32_t ttx_mag_number;
  int32_t ttx_page_number;
  int32_t dvb_ttx_enabled;
  int32_t dvb_ttx_pid;
  int32_t dvb_vbi_enabled;
  int32_t dvb_vbi_pid;
  int32_t dvb_vbi_ttx;
  int32_t dvb_vbi_inverted_ttx;
  int32_t dvb_vbi_vps;
  int32_t dvb_vbi_wss;
};
#define OBED__ANCILLARY_OPTS__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&obed__ancillary_opts__descriptor) \
    , 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }


struct  _Obed__MuxOpts
{
  ProtobufCMessage base;
  int32_t muxrate;
  int32_t ts_type;
  int32_t null_packets;
  int32_t pcr_pid;
  int32_t pmt_pid;
  int32_t program_num;
  int32_t ts_id;
  int32_t pat_period;
  int32_t pcr_period;
  char *service_name;
  char *program_name;
};
#define OBED__MUX_OPTS__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&obed__mux_opts__descriptor) \
    , 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL }


struct  _Obed__OutputOpts
{
  ProtobufCMessage base;
  int32_t method;
  char *ip_address;
  int32_t port;
  int32_t ttl;
  char *miface;
};
#define OBED__OUTPUT_OPTS__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&obed__output_opts__descriptor) \
    , 0, NULL, 0, 0, NULL }


struct  _Obed__EncoderControl
{
  ProtobufCMessage base;
  char *encoder_action;
  int32_t control_version;
  Obed__InputOpts *input_opts;
  Obed__VideoOpts *video_opts;
  size_t n_audio_opts;
  Obed__AudioOpts **audio_opts;
  Obed__AncillaryOpts *ancillary_opts;
  Obed__MuxOpts *mux_opts;
  size_t n_output_opts;
  Obed__OutputOpts **output_opts;
};
#define OBED__ENCODER_CONTROL__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&obed__encoder_control__descriptor) \
    , NULL, 0, NULL, NULL, 0,NULL, NULL, NULL, 0,NULL }


struct  _Obed__EncoderResponse
{
  ProtobufCMessage base;
  char *encoder_response;
};
#define OBED__ENCODER_RESPONSE__INIT \
 { PROTOBUF_C_MESSAGE_INIT (&obed__encoder_response__descriptor) \
    , NULL }


/* Obed__InputOpts methods */
void   obed__input_opts__init
                     (Obed__InputOpts         *message);
size_t obed__input_opts__get_packed_size
                     (const Obed__InputOpts   *message);
size_t obed__input_opts__pack
                     (const Obed__InputOpts   *message,
                      uint8_t             *out);
size_t obed__input_opts__pack_to_buffer
                     (const Obed__InputOpts   *message,
                      ProtobufCBuffer     *buffer);
Obed__InputOpts *
       obed__input_opts__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   obed__input_opts__free_unpacked
                     (Obed__InputOpts *message,
                      ProtobufCAllocator *allocator);
/* Obed__VideoOpts methods */
void   obed__video_opts__init
                     (Obed__VideoOpts         *message);
size_t obed__video_opts__get_packed_size
                     (const Obed__VideoOpts   *message);
size_t obed__video_opts__pack
                     (const Obed__VideoOpts   *message,
                      uint8_t             *out);
size_t obed__video_opts__pack_to_buffer
                     (const Obed__VideoOpts   *message,
                      ProtobufCBuffer     *buffer);
Obed__VideoOpts *
       obed__video_opts__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   obed__video_opts__free_unpacked
                     (Obed__VideoOpts *message,
                      ProtobufCAllocator *allocator);
/* Obed__AudioOpts methods */
void   obed__audio_opts__init
                     (Obed__AudioOpts         *message);
size_t obed__audio_opts__get_packed_size
                     (const Obed__AudioOpts   *message);
size_t obed__audio_opts__pack
                     (const Obed__AudioOpts   *message,
                      uint8_t             *out);
size_t obed__audio_opts__pack_to_buffer
                     (const Obed__AudioOpts   *message,
                      ProtobufCBuffer     *buffer);
Obed__AudioOpts *
       obed__audio_opts__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   obed__audio_opts__free_unpacked
                     (Obed__AudioOpts *message,
                      ProtobufCAllocator *allocator);
/* Obed__AncillaryOpts methods */
void   obed__ancillary_opts__init
                     (Obed__AncillaryOpts         *message);
size_t obed__ancillary_opts__get_packed_size
                     (const Obed__AncillaryOpts   *message);
size_t obed__ancillary_opts__pack
                     (const Obed__AncillaryOpts   *message,
                      uint8_t             *out);
size_t obed__ancillary_opts__pack_to_buffer
                     (const Obed__AncillaryOpts   *message,
                      ProtobufCBuffer     *buffer);
Obed__AncillaryOpts *
       obed__ancillary_opts__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   obed__ancillary_opts__free_unpacked
                     (Obed__AncillaryOpts *message,
                      ProtobufCAllocator *allocator);
/* Obed__MuxOpts methods */
void   obed__mux_opts__init
                     (Obed__MuxOpts         *message);
size_t obed__mux_opts__get_packed_size
                     (const Obed__MuxOpts   *message);
size_t obed__mux_opts__pack
                     (const Obed__MuxOpts   *message,
                      uint8_t             *out);
size_t obed__mux_opts__pack_to_buffer
                     (const Obed__MuxOpts   *message,
                      ProtobufCBuffer     *buffer);
Obed__MuxOpts *
       obed__mux_opts__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   obed__mux_opts__free_unpacked
                     (Obed__MuxOpts *message,
                      ProtobufCAllocator *allocator);
/* Obed__OutputOpts methods */
void   obed__output_opts__init
                     (Obed__OutputOpts         *message);
size_t obed__output_opts__get_packed_size
                     (const Obed__OutputOpts   *message);
size_t obed__output_opts__pack
                     (const Obed__OutputOpts   *message,
                      uint8_t             *out);
size_t obed__output_opts__pack_to_buffer
                     (const Obed__OutputOpts   *message,
                      ProtobufCBuffer     *buffer);
Obed__OutputOpts *
       obed__output_opts__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   obed__output_opts__free_unpacked
                     (Obed__OutputOpts *message,
                      ProtobufCAllocator *allocator);
/* Obed__EncoderControl methods */
void   obed__encoder_control__init
                     (Obed__EncoderControl         *message);
size_t obed__encoder_control__get_packed_size
                     (const Obed__EncoderControl   *message);
size_t obed__encoder_control__pack
                     (const Obed__EncoderControl   *message,
                      uint8_t             *out);
size_t obed__encoder_control__pack_to_buffer
                     (const Obed__EncoderControl   *message,
                      ProtobufCBuffer     *buffer);
Obed__EncoderControl *
       obed__encoder_control__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   obed__encoder_control__free_unpacked
                     (Obed__EncoderControl *message,
                      ProtobufCAllocator *allocator);
/* Obed__EncoderResponse methods */
void   obed__encoder_response__init
                     (Obed__EncoderResponse         *message);
size_t obed__encoder_response__get_packed_size
                     (const Obed__EncoderResponse   *message);
size_t obed__encoder_response__pack
                     (const Obed__EncoderResponse   *message,
                      uint8_t             *out);
size_t obed__encoder_response__pack_to_buffer
                     (const Obed__EncoderResponse   *message,
                      ProtobufCBuffer     *buffer);
Obed__EncoderResponse *
       obed__encoder_response__unpack
                     (ProtobufCAllocator  *allocator,
                      size_t               len,
                      const uint8_t       *data);
void   obed__encoder_response__free_unpacked
                     (Obed__EncoderResponse *message,
                      ProtobufCAllocator *allocator);
/* --- per-message closures --- */

typedef void (*Obed__InputOpts_Closure)
                 (const Obed__InputOpts *message,
                  void *closure_data);
typedef void (*Obed__VideoOpts_Closure)
                 (const Obed__VideoOpts *message,
                  void *closure_data);
typedef void (*Obed__AudioOpts_Closure)
                 (const Obed__AudioOpts *message,
                  void *closure_data);
typedef void (*Obed__AncillaryOpts_Closure)
                 (const Obed__AncillaryOpts *message,
                  void *closure_data);
typedef void (*Obed__MuxOpts_Closure)
                 (const Obed__MuxOpts *message,
                  void *closure_data);
typedef void (*Obed__OutputOpts_Closure)
                 (const Obed__OutputOpts *message,
                  void *closure_data);
typedef void (*Obed__EncoderControl_Closure)
                 (const Obed__EncoderControl *message,
                  void *closure_data);
typedef void (*Obed__EncoderResponse_Closure)
                 (const Obed__EncoderResponse *message,
                  void *closure_data);

/* --- services --- */

typedef struct _Obed__EncoderConfig_Service Obed__EncoderConfig_Service;
struct _Obed__EncoderConfig_Service
{
  ProtobufCService base;
  void (*encoder_config)(Obed__EncoderConfig_Service *service,
                         const Obed__EncoderControl *input,
                         Obed__EncoderResponse_Closure closure,
                         void *closure_data);
};
typedef void (*Obed__EncoderConfig_ServiceDestroy)(Obed__EncoderConfig_Service *);
void obed__encoder_config__init (Obed__EncoderConfig_Service *service,
                                 Obed__EncoderConfig_ServiceDestroy destroy);
#define OBED__ENCODER_CONFIG__BASE_INIT \
    { &obed__encoder_config__descriptor, protobuf_c_service_invoke_internal, NULL }
#define OBED__ENCODER_CONFIG__INIT(function_prefix__) \
    { OBED__ENCODER_CONFIG__BASE_INIT,\
      function_prefix__ ## encoder_config  }
void obed__encoder_config__encoder_config(ProtobufCService *service,
                                          const Obed__EncoderControl *input,
                                          Obed__EncoderResponse_Closure closure,
                                          void *closure_data);

/* --- descriptors --- */

extern const ProtobufCMessageDescriptor obed__input_opts__descriptor;
extern const ProtobufCMessageDescriptor obed__video_opts__descriptor;
extern const ProtobufCMessageDescriptor obed__audio_opts__descriptor;
extern const ProtobufCMessageDescriptor obed__ancillary_opts__descriptor;
extern const ProtobufCMessageDescriptor obed__mux_opts__descriptor;
extern const ProtobufCMessageDescriptor obed__output_opts__descriptor;
extern const ProtobufCMessageDescriptor obed__encoder_control__descriptor;
extern const ProtobufCMessageDescriptor obed__encoder_response__descriptor;
extern const ProtobufCServiceDescriptor obed__encoder_config__descriptor;

PROTOBUF_C_END_DECLS


#endif  /* PROTOBUF_obed_2eproto__INCLUDED */
