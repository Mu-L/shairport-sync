/*
 * Apple RTP protocol handler. This file is part of Shairport.
 * Copyright (c) James Laird 2013
 * Copyright (c) Mike Brady 2014--2025
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "rtp.h"
#include "common.h"
#include "player.h"
#include "rtsp.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <memory.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef CONFIG_AIRPLAY_2
// #include "plist_xml_strings.h"
#include "ptp-utilities.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <sodium.h>
#endif

struct Nvll {
  char *name;
  double value;
  struct Nvll *next;
};

typedef struct Nvll nvll;

uint64_t local_to_remote_time_jitter;
uint64_t local_to_remote_time_jitter_count;

typedef struct {
  int closed;
  int error_code;
  int sock_fd;
  char *buffer;
  char *toq;
  char *eoq;
  size_t buffer_max_size;
  size_t buffer_occupancy;
  pthread_mutex_t mutex;
  pthread_cond_t not_empty_cv;
  pthread_cond_t not_full_cv;
} buffered_tcp_desc;

typedef struct {
  char *buf;
  size_t buf_size;
  size_t buf_pos;
} structured_buffer;

structured_buffer *sbuf_new(size_t size) {
  structured_buffer *sbuf = (structured_buffer *)malloc(sizeof(structured_buffer));
  if (sbuf != NULL) {
    memset(sbuf, 0, sizeof(structured_buffer));
    char *buf = malloc(size + 1); // extra space for a possible NULL
    if (buf == NULL) {
      free(sbuf);
      sbuf = NULL;
    } else {
      sbuf->buf_size = size;
      sbuf->buf = buf;
    }
  }
  return sbuf;
}

int sbuf_clear(structured_buffer *sbuf) {
  int response = -1;
  if ((sbuf != NULL) && (sbuf->buf != NULL)) {
    sbuf->buf_pos = 0;
    response = 0;
  }
  return response;
}

void sbuf_free(structured_buffer *sbuf) {
  if (sbuf != NULL) {
    if (sbuf->buf != NULL)
      free(sbuf->buf);
    free(sbuf);
  }
}

void sbuf_cleanup(void *arg) {
  structured_buffer *sbuf = (structured_buffer *)arg;
  debug(3, "structured_buffer cleanup");
  sbuf_free(sbuf);
}

int sbuf_printf(structured_buffer *sbuf, const char *format, ...) {
  int response = -1;
  if ((sbuf != NULL) && (sbuf->buf != NULL)) {
    char *p = sbuf->buf + sbuf->buf_pos;
    va_list args;
    va_start(args, format);
    vsnprintf(p, sbuf->buf_size - sbuf->buf_pos, format, args);
    sbuf->buf_pos = sbuf->buf_pos + strlen(p);
    response = strlen(p);
    va_end(args);
  }
  return response;
}

int sbuf_append(structured_buffer *sbuf, char *plistString, uint32_t plistStringLength) {
  int response = -1;
  if ((sbuf != NULL) && (sbuf->buf != NULL) && (plistString != NULL)) {
    if (plistStringLength == 0) {
      response = 0;
    } else {
      if (plistStringLength < (sbuf->buf_size - sbuf->buf_pos)) {
        memcpy(sbuf->buf + sbuf->buf_pos, plistString, plistStringLength);
        sbuf->buf_pos = sbuf->buf_pos + plistStringLength;
        response = 0;
      } else {
        debug(1, "plist too large -- omitted");
      }
    }
  }
  return response;
}

int sbuf_buf_and_length(structured_buffer *sbuf, char **b, size_t *l) {
  int response = 0;
  if ((sbuf != NULL) && (sbuf->buf != NULL)) {
    *b = sbuf->buf;
    *l = sbuf->buf_pos;
  } else {
    response = -1;
  }
  return response;
}

/*
      char obf[4096];
      char *obfp = obf;
      size_t obfc;
      for (obfc=0; obfc < strlen(buffer); obfc++) {
        snprintf(obfp, 3, "%02X", buffer[obfc]);
        obfp+=2;
      };
      *obfp=0;
      debug(1,"Writing: \"%s\"",obf);

*/

void check64conversion(const char *prompt, const uint8_t *source, uint64_t value) {
  char converted_value[128];
  sprintf(converted_value, "%" PRIx64 "", value);

  char obf[32];
  char *obfp = obf;
  int obfc;
  int suppress_zeroes = 1;
  for (obfc = 0; obfc < 8; obfc++) {
    if ((suppress_zeroes == 0) || (source[obfc] != 0)) {
      if (suppress_zeroes != 0) {
        if (source[obfc] < 0x10) {
          snprintf(obfp, 3, "%1x", source[obfc]);
          obfp += 1;
        } else {
          snprintf(obfp, 3, "%02x", source[obfc]);
          obfp += 2;
        }
      } else {
        snprintf(obfp, 3, "%02x", source[obfc]);
        obfp += 2;
      }
      suppress_zeroes = 0;
    }
  };
  *obfp = 0;
  if (strcmp(converted_value, obf) != 0) {
    debug(1, "%s check64conversion error converting \"%s\" to %" PRIx64 ".", prompt, obf, value);
  }
}

void check32conversion(const char *prompt, const uint8_t *source, uint32_t value) {
  char converted_value[128];
  sprintf(converted_value, "%" PRIx32 "", value);

  char obf[32];
  char *obfp = obf;
  int obfc;
  int suppress_zeroes = 1;
  for (obfc = 0; obfc < 4; obfc++) {
    if ((suppress_zeroes == 0) || (source[obfc] != 0)) {
      if (suppress_zeroes != 0) {
        if (source[obfc] < 0x10) {
          snprintf(obfp, 3, "%1x", source[obfc]);
          obfp += 1;
        } else {
          snprintf(obfp, 3, "%02x", source[obfc]);
          obfp += 2;
        }
      } else {
        snprintf(obfp, 3, "%02x", source[obfc]);
        obfp += 2;
      }
      suppress_zeroes = 0;
    }
  };
  *obfp = 0;
  if (strcmp(converted_value, obf) != 0) {
    debug(1, "%s check32conversion error converting \"%s\" to %" PRIx32 ".", prompt, obf, value);
  }
}

void rtp_initialise(rtsp_conn_info *conn) {
  conn->rtp_time_of_last_resend_request_error_ns = 0;
  conn->rtp_running = 0;
  // initialise the timer mutex
  int rc = pthread_mutex_init(&conn->reference_time_mutex, NULL);
  if (rc)
    debug(1, "Error initialising reference_time_mutex.");
}

void rtp_terminate(rtsp_conn_info *conn) {
  conn->anchor_rtptime = 0;
  // destroy the timer mutex
  int rc = pthread_mutex_destroy(&conn->reference_time_mutex);
  if (rc)
    debug(1, "Error destroying reference_time_mutex variable.");
}

uint64_t local_to_remote_time_difference_now(rtsp_conn_info *conn) {
  // this is an attempt to compensate for clock drift since the last time ping that was used
  // so, if we have a non-zero clock drift, we will calculate the drift there would
  // be from the time of the last time ping
  uint64_t time_since_last_local_to_remote_time_difference_measurement =
      get_absolute_time_in_ns() - conn->local_to_remote_time_difference_measurement_time;

  uint64_t result = conn->local_to_remote_time_difference;
  if (conn->local_to_remote_time_gradient >= 1.0) {
    result = conn->local_to_remote_time_difference +
             (uint64_t)((conn->local_to_remote_time_gradient - 1.0) *
                        time_since_last_local_to_remote_time_difference_measurement);
  } else {
    result = conn->local_to_remote_time_difference -
             (uint64_t)((1.0 - conn->local_to_remote_time_gradient) *
                        time_since_last_local_to_remote_time_difference_measurement);
  }
  return result;
}

void rtp_audio_receiver_cleanup_handler(__attribute__((unused)) void *arg) {
  debug(3, "Audio Receiver Cleanup Done.");
}

void *rtp_audio_receiver(void *arg) {
  //  #include <syscall.h>
  //  debug(1, "rtp_audio_receiver PID %d", syscall(SYS_gettid));
  pthread_cleanup_push(rtp_audio_receiver_cleanup_handler, arg);
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;

  int32_t last_seqno = -1;
  uint8_t packet[2048], *pktp;

  uint64_t time_of_previous_packet_ns = 0;
  float longest_packet_time_interval_us = 0.0;

  // mean and variance calculations from "online_variance" algorithm at
  // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online_algorithm

  int32_t stat_n = 0;
  float stat_mean = 0.0;
  float stat_M2 = 0.0;

  ssize_t nread;
  while (1) {
    nread = recv(conn->audio_socket, packet, sizeof(packet), 0);

    uint64_t local_time_now_ns = get_absolute_time_in_ns();
    if (time_of_previous_packet_ns) {
      float time_interval_us = (local_time_now_ns - time_of_previous_packet_ns) * 0.001;
      time_of_previous_packet_ns = local_time_now_ns;
      if (time_interval_us > longest_packet_time_interval_us)
        longest_packet_time_interval_us = time_interval_us;
      stat_n += 1;
      float stat_delta = time_interval_us - stat_mean;
      stat_mean += stat_delta / stat_n;
      stat_M2 += stat_delta * (time_interval_us - stat_mean);
      if ((stat_n != 1) && (stat_n % 2500 == 0)) {
        debug(2,
              "Packet reception interval stats: mean, standard deviation and max for the last "
              "2,500 packets in microseconds: %10.1f, %10.1f, %10.1f.",
              stat_mean, sqrtf(stat_M2 / (stat_n - 1)), longest_packet_time_interval_us);
        stat_n = 0;
        stat_mean = 0.0;
        stat_M2 = 0.0;
        time_of_previous_packet_ns = 0;
        longest_packet_time_interval_us = 0.0;
      }
    } else {
      time_of_previous_packet_ns = local_time_now_ns;
    }

    if (nread >= 0) {
      ssize_t plen = nread;
      uint8_t type = packet[1] & ~0x80;
      if (type == 0x60 || type == 0x56) { // audio data / resend
        pktp = packet;
        if (type == 0x56) {
          pktp += 4;
          plen -= 4;
        }
        seq_t seqno = ntohs(*(uint16_t *)(pktp + 2));
        // increment last_seqno and see if it's the same as the incoming seqno

        if (type == 0x60) { // regular audio data

          /*
          char obf[4096];
          char *obfp = obf;
          int obfc;
          for (obfc=0;obfc<plen;obfc++) {
            snprintf(obfp, 3, "%02X", pktp[obfc]);
            obfp+=2;
          };
          *obfp=0;
          debug(1,"Audio Packet Received: \"%s\"",obf);
          */

          if (last_seqno == -1)
            last_seqno = seqno;
          else {
            last_seqno = (last_seqno + 1) & 0xffff;
            // if (seqno != last_seqno)
            //  debug(3, "RTP: Packets out of sequence: expected: %d, got %d.", last_seqno, seqno);
            last_seqno = seqno; // reset warning...
          }
        } else {
          debug(3, "Audio Receiver -- Retransmitted Audio Data Packet %u received.", seqno);
        }

        uint32_t actual_timestamp = ntohl(*(uint32_t *)(pktp + 4));

        // uint32_t ssid = ntohl(*(uint32_t *)(pktp + 8));
        // debug(1, "Audio packet SSID: %08X,%u", ssid,ssid);

        // if (packet[1]&0x10)
        //	debug(1,"Audio packet Extension bit set.");

        pktp += 12;
        plen -= 12;

        // check if packet contains enough content to be reasonable
        if (plen >= 16) {
          if ((config.diagnostic_drop_packet_fraction == 0.0) ||
              (drand48() > config.diagnostic_drop_packet_fraction))
            player_put_packet(ALAC_44100_S16_2, seqno, actual_timestamp, pktp, plen, 0, 0,
                              conn); // original format, no mute, not discontinuous
          else
            debug(3, "Dropping audio packet %u to simulate a bad connection.", seqno);
          continue;
        }
        if (type == 0x56 && seqno == 0) {
          debug(2, "resend-related request packet received, ignoring.");
          continue;
        }
        debug(1, "Audio receiver -- Unknown RTP packet of type 0x%02X length %d seqno %d", type,
              nread, seqno);
      }
      warn("Audio receiver -- Unknown RTP packet of type 0x%02X length %d.", type, nread);
    } else {
      char em[1024];
      strerror_r(errno, em, sizeof(em));
      debug(1, "Error %d receiving an audio packet: \"%s\".", errno, em);
    }
  }

  /*
  debug(3, "Audio receiver -- Server RTP thread interrupted. terminating.");
  close(conn->audio_socket);
  */

  debug(1, "Audio receiver thread \"normal\" exit -- this can't happen. Hah!");
  pthread_cleanup_pop(0); // don't execute anything here.
  debug(2, "Audio receiver thread exit.");
  pthread_exit(NULL);
}

void rtp_control_handler_cleanup_handler(__attribute__((unused)) void *arg) {
  debug(2, "Control Receiver Cleanup Done.");
}

void *rtp_control_receiver(void *arg) {
  //  #include <syscall.h>
  //  debug(1, "rtp_control_receiver PID %d", syscall(SYS_gettid));
  pthread_cleanup_push(rtp_control_handler_cleanup_handler, arg);
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;

  conn->anchor_rtptime = 0; // nothing valid received yet
  uint8_t packet[2048], *pktp;
  // struct timespec tn;
  uint64_t remote_time_of_sync;
  uint32_t sync_rtp_timestamp;
  ssize_t nread;
  while (1) {
    nread = recv(conn->control_socket, packet, sizeof(packet), 0);
    if (nread >= 0) {
      if ((config.diagnostic_drop_packet_fraction == 0.0) ||
          (drand48() > config.diagnostic_drop_packet_fraction)) {

        ssize_t plen = nread;
        if (packet[1] == 0xd4) { // sync data
          // clang-format off
            /*
                // the following stanza is for debugging only -- normally commented out.
                {
                  char obf[4096];
                  char *obfp = obf;
                  int obfc;
                  for (obfc = 0; obfc < plen; obfc++) {
                    snprintf(obfp, 3, "%02X", packet[obfc]);
                    obfp += 2;
                  };
                  *obfp = 0;


                  // get raw timestamp information
                  // I think that a good way to understand these timestamps is that
                  // (1) the rtlt below is the timestamp of the frame that should be playing at the
                  // client-time specified in the packet if there was no delay
                  // and (2) that the rt below is the timestamp of the frame that should be playing
                  // at the client-time specified in the packet on this device taking account of
                  // the delay
                  // Thus, (3) the latency can be calculated by subtracting the second from the
                  // first.
                  // There must be more to it -- there something missing.

                  // In addition, it seems that if the value of the short represented by the second
                  // pair of bytes in the packet is 7
                  // then an extra time lag is expected to be added, presumably by
                  // the AirPort Express.

                  // Best guess is that this delay is 11,025 frames.

                  uint32_t rtlt = nctohl(&packet[4]); // raw timestamp less latency
                  uint32_t rt = nctohl(&packet[16]);  // raw timestamp

                  uint32_t fl = nctohs(&packet[2]); //

                  debug(1,"Sync Packet of %d bytes received: \"%s\", flags: %d, timestamps %u and %u,
              giving a latency of %d frames.",plen,obf,fl,rt,rtlt,rt-rtlt);
                  //debug(1,"Monotonic timestamps are: %" PRId64 " and %" PRId64 "
              respectively.",monotonic_timestamp(rt, conn),monotonic_timestamp(rtlt, conn));
                }
            */
          // clang-format on
          if (conn->local_to_remote_time_difference) { // need a time packet to be interchanged
                                                       // first...
            uint64_t ps, pn;

            ps = nctohl(&packet[8]);
            ps = ps * 1000000000; // this many nanoseconds from the whole seconds
            pn = nctohl(&packet[12]);
            pn = pn * 1000000000;
            pn = pn >> 32; // this many nanoseconds from the fractional part
            remote_time_of_sync = ps + pn;

            // debug(1,"Remote Sync Time: " PRIu64 "",remote_time_of_sync);

            sync_rtp_timestamp = nctohl(&packet[16]);
            uint32_t rtp_timestamp_less_latency = nctohl(&packet[4]);

            // debug(1,"Sync timestamp is %u.",ntohl(*((uint32_t *)&packet[16])));

            if (config.userSuppliedLatency) {
              if (config.userSuppliedLatency != conn->latency) {
                debug(1, "Using the user-supplied latency: %" PRIu32 ".",
                      config.userSuppliedLatency);
              }
              conn->latency = config.userSuppliedLatency;
            } else {

              // It seems that the second pair of bytes in the packet indicate whether a fixed
              // delay of 11,025 frames should be added -- iTunes set this field to 7 and
              // AirPlay sets it to 4.

              // However, on older versions of AirPlay, the 11,025 frames seem to be necessary too

              // The value of 11,025 (0.25 seconds) is a guess based on the "Audio-Latency"
              // parameter
              // returned by an AE.

              // Sigh, it would be nice to have a published protocol...

              uint16_t flags = nctohs(&packet[2]);
              uint32_t la = sync_rtp_timestamp - rtp_timestamp_less_latency; // note, this might
                                                                             // loop around in
                                                                             // modulo. Not sure if
                                                                             // you'll get an error!
              // debug(1, "Latency from the sync packet is %" PRIu32 " frames.", la);

              if ((flags == 7) || ((conn->AirPlayVersion > 0) && (conn->AirPlayVersion <= 353)) ||
                  ((conn->AirPlayVersion > 0) && (conn->AirPlayVersion >= 371))) {
                la += config.fixedLatencyOffset;
                // debug(1, "Latency offset by %" PRIu32" frames due to the source flags and
                // version giving a latency of %" PRIu32 " frames.", config.fixedLatencyOffset,
                // la);
              }
              if ((conn->maximum_latency) && (conn->maximum_latency < la))
                la = conn->maximum_latency;
              if ((conn->minimum_latency) && (conn->minimum_latency > la))
                la = conn->minimum_latency;

              const uint32_t max_frames = ((3 * BUFFER_FRAMES * 352) / 4) - 11025;

              if (la > max_frames) {
                warn("An out-of-range latency request of %" PRIu32
                     " frames was ignored. Must be %" PRIu32
                     " frames or less (44,100 frames per second). "
                     "Latency remains at %" PRIu32 " frames.",
                     la, max_frames, conn->latency);
              } else {

                // here we have the latency but it does not yet account for the
                // audio_backend_latency_offset
                int32_t latency_offset =
                    (int32_t)(config.audio_backend_latency_offset * conn->input_rate);

                // debug(1,"latency offset is %" PRId32 ", input rate is %u", latency_offset,
                // conn->input_rate);
                int32_t adjusted_latency = latency_offset + (int32_t)la;
                if ((adjusted_latency < 0) ||
                    (adjusted_latency >
                     (int32_t)(conn->frames_per_packet *
                               (BUFFER_FRAMES - config.minimum_free_buffer_headroom))))
                  warn("audio_backend_latency_offset out of range -- ignored.");
                else
                  la = adjusted_latency;

                if (la != conn->latency) {
                  conn->latency = la;
                  debug(2,
                        "New latency: %" PRIu32 ", sync latency: %" PRIu32
                        ", minimum latency: %" PRIu32 ", maximum "
                        "latency: %" PRIu32 ", fixed offset: %" PRIu32
                        ", audio_backend_latency_offset: %f.",
                        conn->latency, sync_rtp_timestamp - rtp_timestamp_less_latency,
                        conn->minimum_latency, conn->maximum_latency, config.fixedLatencyOffset,
                        config.audio_backend_latency_offset);
                }
              }
            }

            // here, we apply the latency to the sync_rtp_timestamp

            sync_rtp_timestamp = sync_rtp_timestamp - conn->latency;

            debug_mutex_lock(&conn->reference_time_mutex, 1000, 0);

            if (conn->initial_reference_time == 0) {
              if (conn->packet_count_since_flush > 0) {
                conn->initial_reference_time = remote_time_of_sync;
                conn->initial_reference_timestamp = sync_rtp_timestamp;
              }
            } else {
              uint64_t remote_frame_time_interval =
                  conn->anchor_time -
                  conn->initial_reference_time; // here, this should never be zero
              if (remote_frame_time_interval) {
                conn->remote_frame_rate =
                    (1.0E9 * (conn->anchor_rtptime - conn->initial_reference_timestamp)) /
                    remote_frame_time_interval;
              } else {
                conn->remote_frame_rate = 0.0; // use as a flag.
              }
            }

            // this is for debugging
            uint64_t old_remote_reference_time = conn->anchor_time;
            uint32_t old_reference_timestamp = conn->anchor_rtptime;
            // int64_t old_latency_delayed_timestamp = conn->latency_delayed_timestamp;
            if (conn->anchor_remote_info_is_valid != 0) {
              int64_t time_difference = remote_time_of_sync - conn->anchor_time;
              int32_t frame_difference = sync_rtp_timestamp - conn->anchor_rtptime;
              double time_difference_in_frames =
                  (1.0 * time_difference * conn->input_rate) / 1000000000;
              double frame_change = frame_difference - time_difference_in_frames;
              debug(2,
                    "AP1 control thread: set_ntp_anchor_info: rtptime: %" PRIu32
                    ", networktime: %" PRIx64 ", frame adjustment: %7.3f.",
                    sync_rtp_timestamp, remote_time_of_sync, frame_change);
            } else {
              debug(2,
                    "AP1 control thread: set_ntp_anchor_info: rtptime: %" PRIu32
                    ", networktime: %" PRIx64 ".",
                    sync_rtp_timestamp, remote_time_of_sync);
            }

            conn->anchor_time = remote_time_of_sync;
            // conn->reference_timestamp_time =
            //    remote_time_of_sync - local_to_remote_time_difference_now(conn);
            conn->anchor_rtptime = sync_rtp_timestamp;
            conn->anchor_remote_info_is_valid = 1;

            conn->latency_delayed_timestamp = rtp_timestamp_less_latency;
            debug_mutex_unlock(&conn->reference_time_mutex, 0);

            conn->reference_to_previous_time_difference =
                remote_time_of_sync - old_remote_reference_time;
            if (old_reference_timestamp == 0)
              conn->reference_to_previous_frame_difference = 0;
            else
              conn->reference_to_previous_frame_difference =
                  sync_rtp_timestamp - old_reference_timestamp;
          } else {
            debug(2, "Sync packet received before we got a timing packet back.");
          }
        } else if (packet[1] == 0xd6) { // resent audio data in the control path -- whaale only?
          pktp = packet + 4;
          plen -= 4;
          seq_t seqno = ntohs(*(uint16_t *)(pktp + 2));
          debug(3, "Control Receiver -- Retransmitted Audio Data Packet %u received.", seqno);

          uint32_t actual_timestamp = ntohl(*(uint32_t *)(pktp + 4));

          pktp += 12;
          plen -= 12;

          // check if packet contains enough content to be reasonable
          if (plen >= 16) {
            // i.e. ssrc, sequence number, timestamp, data, data_length_in_bytes, mute,
            // discontinuous, conn
            player_put_packet(ALAC_44100_S16_2, seqno, actual_timestamp, pktp, plen, 0, 0,
                              conn); // original format, no mute, not discontinuous
            continue;
          } else {
            debug(3, "Too-short retransmitted audio packet received in control port, ignored.");
          }
        } else
          debug(1, "Control Receiver -- Unknown RTP packet of type 0x%02X length %d, ignored.",
                packet[1], nread);
      } else {
        debug(3, "Control Receiver -- dropping a packet to simulate a bad network.");
      }
    } else {

      char em[1024];
      strerror_r(errno, em, sizeof(em));
      debug(1, "Control Receiver -- error %d receiving a packet: \"%s\".", errno, em);
    }
  }
  debug(1, "Control RTP thread \"normal\" exit -- this can't happen. Hah!");
  pthread_cleanup_pop(0); // don't execute anything here.
  debug(2, "Control RTP thread exit.");
  pthread_exit(NULL);
}

void rtp_timing_sender_cleanup_handler(void *arg) {
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  debug(3, "Connection %d: Timing Sender Cleanup.", conn->connection_number);
}

void *rtp_timing_sender(void *arg) {
  //  #include <syscall.h>
  //  debug(1, "rtp_timing_sender PID %d", syscall(SYS_gettid));
  pthread_cleanup_push(rtp_timing_sender_cleanup_handler, arg);
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  struct timing_request {
    char leader;
    char type;
    uint16_t seqno;
    uint32_t filler;
    uint64_t origin, receive, transmit;
  };

  uint64_t request_number = 0;

  struct timing_request req; // *not* a standard RTCP NACK

  req.leader = 0x80;
  req.type = 0xd2; // Timing request
  req.filler = 0;
  req.seqno = htons(7);

  conn->time_ping_count = 0;
  while (1) {
    if (conn->udp_clock_sender_is_initialised == 0) {
      request_number = 0;
      conn->udp_clock_sender_is_initialised = 1;
      debug(2, "AP1 clock sender thread: initialised.");
    }
    // debug(1,"Send a timing request");

    if (!conn->rtp_running)
      debug(1, "rtp_timing_sender called without active stream in RTSP conversation thread %d!",
            conn->connection_number);

    // debug(1, "Requesting ntp timestamp exchange.");

    req.filler = 0;
    req.origin = req.receive = req.transmit = 0;

    conn->departure_time = get_absolute_time_in_ns();
    socklen_t msgsize = sizeof(struct sockaddr_in);
#ifdef AF_INET6
    if (conn->rtp_client_timing_socket.SAFAMILY == AF_INET6) {
      msgsize = sizeof(struct sockaddr_in6);
    }
#endif
    if ((config.diagnostic_drop_packet_fraction == 0.0) ||
        (drand48() > config.diagnostic_drop_packet_fraction)) {
      if (sendto(conn->timing_socket, &req, sizeof(req), 0,
                 (struct sockaddr *)&conn->rtp_client_timing_socket, msgsize) == -1) {
        char em[1024];
        strerror_r(errno, em, sizeof(em));
        debug(1, "Error %d using send-to to the timing socket: \"%s\".", errno, em);
      }
    } else {
      debug(3, "Timing Sender Thread -- dropping outgoing packet to simulate bad network.");
    }

    request_number++;

    if (request_number <= 3)
      usleep(300000); // these are thread cancellation points
    else
      usleep(3000000);
  }
  debug(3, "rtp_timing_sender thread interrupted. This should never happen.");
  pthread_cleanup_pop(0); // don't execute anything here.
  pthread_exit(NULL);
}

void rtp_timing_receiver_cleanup_handler(void *arg) {
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  debug(3, "Timing Receiver Cleanup.");
  // walk down the list of DACP / gradient pairs, if any
  nvll *gradients = config.gradients;
  if (conn->dacp_id)
    while ((gradients) && (strcasecmp((const char *)&conn->client_ip_string, gradients->name) != 0))
      gradients = gradients->next;

  // if gradients comes out of this non-null, it is pointing to the DACP and it's last-known
  // gradient
  if (gradients) {
    gradients->value = conn->local_to_remote_time_gradient;
    // debug(1,"Updating a drift of %.2f ppm for \"%s\".", (conn->local_to_remote_time_gradient
    // - 1.0)*1000000, gradients->name);
  } else {
    nvll *new_entry = (nvll *)malloc(sizeof(nvll));
    if (new_entry) {
      new_entry->name = strdup((const char *)&conn->client_ip_string);
      new_entry->value = conn->local_to_remote_time_gradient;
      new_entry->next = config.gradients;
      config.gradients = new_entry;
      // debug(1,"Setting a new drift of %.2f ppm for \"%s\".", (conn->local_to_remote_time_gradient
      // - 1.0)*1000000, new_entry->name);
    }
  }

  debug(3, "Cancel Timing Requester.");
  pthread_cancel(conn->timer_requester);
  int oldState;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState);
  debug(3, "Join Timing Requester.");
  pthread_join(conn->timer_requester, NULL);
  debug(3, "Timing Receiver Cleanup Successful.");
  pthread_setcancelstate(oldState, NULL);
}

void *rtp_timing_receiver(void *arg) {
  //  #include <syscall.h>
  //  debug(1, "rtp_timing_receiver PID %d", syscall(SYS_gettid));
  pthread_cleanup_push(rtp_timing_receiver_cleanup_handler, arg);
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;

  uint8_t packet[2048];
  ssize_t nread;
  named_pthread_create(&conn->timer_requester, NULL, &rtp_timing_sender, arg, "ap1_tim_req_%d",
                       conn->connection_number);
  //    struct timespec att;
  uint64_t distant_receive_time, distant_transmit_time, arrival_time, return_time;
  local_to_remote_time_jitter = 0;
  local_to_remote_time_jitter_count = 0;

  uint64_t first_local_to_remote_time_difference = 0;

  conn->local_to_remote_time_gradient = 1.0; // initial value.
  // walk down the list of DACP / gradient pairs, if any
  nvll *gradients = config.gradients;
  while ((gradients) && (strcasecmp((const char *)&conn->client_ip_string, gradients->name) != 0))
    gradients = gradients->next;

  // if gradients comes out of this non-null, it is pointing to the IP and it's last-known gradient
  if (gradients) {
    conn->local_to_remote_time_gradient = gradients->value;
    // debug(1,"Using a stored drift of %.2f ppm for \"%s\".", (conn->local_to_remote_time_gradient
    // - 1.0)*1000000, gradients->name);
  }

  // calculate diffusion factor

  // at the end of the array of time pings, the diffusion factor
  // must be diffusion_expansion_factor
  // this, at each step, the diffusion multiplication constant must
  // be the nth root of diffusion_expansion_factor
  // where n is the number of elements in the array

  const double diffusion_expansion_factor = 10;
  double log_of_multiplier = log10(diffusion_expansion_factor) / time_ping_history;
  double multiplier = pow(10, log_of_multiplier);
  uint64_t dispersion_factor = (uint64_t)(multiplier * 100);
  if (dispersion_factor == 0)
    die("dispersion factor is zero!");
  // debug(1,"dispersion factor is %" PRIu64 ".", dispersion_factor);

  // uint64_t first_local_to_remote_time_difference_time;
  // uint64_t l2rtd = 0;
  int sequence_number = 0;

  // for getting mean and sd of return times
  int32_t stat_n = 0;
  double stat_mean = 0.0;
  // double stat_M2 = 0.0;

  while (1) {
    nread = recv(conn->timing_socket, packet, sizeof(packet), 0);
    if (conn->udp_clock_is_initialised == 0) {
      debug(2, "AP1 clock receiver thread: initialised.");
      local_to_remote_time_jitter = 0;
      local_to_remote_time_jitter_count = 0;

      first_local_to_remote_time_difference = 0;

      sequence_number = 0;
      stat_n = 0;
      stat_mean = 0.0;
      conn->udp_clock_is_initialised = 1;
    }
    if (nread >= 0) {
      if ((config.diagnostic_drop_packet_fraction == 0.0) ||
          (drand48() > config.diagnostic_drop_packet_fraction)) {
        arrival_time = get_absolute_time_in_ns();

        // ssize_t plen = nread;
        // debug(1,"Packet Received on Timing Port.");
        if (packet[1] == 0xd3) { // timing reply

          return_time = arrival_time - conn->departure_time;
          debug(2, "clock synchronisation request: return time is %8.3f milliseconds.",
                0.000001 * return_time);

          if (return_time < 200000000) { // must be less than 0.2 seconds
            // distant_receive_time =
            // ((uint64_t)ntohl(*((uint32_t*)&packet[16])))<<32+ntohl(*((uint32_t*)&packet[20]));

            uint64_t ps, pn;

            ps = nctohl(&packet[16]);
            ps = ps * 1000000000; // this many nanoseconds from the whole seconds
            pn = nctohl(&packet[20]);
            pn = pn * 1000000000;
            pn = pn >> 32; // this many nanoseconds from the fractional part
            distant_receive_time = ps + pn;

            // distant_transmit_time =
            // ((uint64_t)ntohl(*((uint32_t*)&packet[24])))<<32+ntohl(*((uint32_t*)&packet[28]));

            ps = nctohl(&packet[24]);
            ps = ps * 1000000000; // this many nanoseconds from the whole seconds
            pn = nctohl(&packet[28]);
            pn = pn * 1000000000;
            pn = pn >> 32; // this many nanoseconds from the fractional part
            distant_transmit_time = ps + pn;

            uint64_t remote_processing_time = 0;

            if (distant_transmit_time >= distant_receive_time)
              remote_processing_time = distant_transmit_time - distant_receive_time;
            else {
              debug(1, "Yikes: distant_transmit_time is before distant_receive_time; remote "
                       "processing time set to zero.");
            }
            // debug(1,"Return trip time: %" PRIu64 " nS, remote processing time: %" PRIu64 "
            // nS.",return_time, remote_processing_time);

            if (remote_processing_time < return_time)
              return_time -= remote_processing_time;
            else
              debug(1, "Remote processing time greater than return time -- ignored.");

            int cc;
            // debug(1, "time ping history is %d entries.", time_ping_history);
            for (cc = time_ping_history - 1; cc > 0; cc--) {
              conn->time_pings[cc] = conn->time_pings[cc - 1];
              // if ((conn->time_ping_count) && (conn->time_ping_count < 10))
              //                conn->time_pings[cc].dispersion =
              //                  conn->time_pings[cc].dispersion * pow(2.14,
              //                  1.0/conn->time_ping_count);
              if (conn->time_pings[cc].dispersion > UINT64_MAX / dispersion_factor)
                debug(1, "dispersion factor is too large at %" PRIu64 ".");
              else
                conn->time_pings[cc].dispersion =
                    (conn->time_pings[cc].dispersion * dispersion_factor) /
                    100; // make the dispersions 'age' by this rational factor
            }
            // these are used for doing a least squares calculation to get the drift
            conn->time_pings[0].local_time = arrival_time;
            conn->time_pings[0].remote_time = distant_transmit_time + return_time / 2;
            conn->time_pings[0].sequence_number = sequence_number++;
            conn->time_pings[0].chosen = 0;
            conn->time_pings[0].dispersion = return_time;
            if (conn->time_ping_count < time_ping_history)
              conn->time_ping_count++;

            // here, calculate the mean and standard deviation of the return times

            // mean and variance calculations from "online_variance" algorithm at
            // https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Online_algorithm

            stat_n += 1;
            double stat_delta = return_time - stat_mean;
            stat_mean += stat_delta / stat_n;
            // stat_M2 += stat_delta * (return_time - stat_mean);
            // debug(1, "Timing packet return time stats: current, mean and standard deviation
            // over %d packets: %.1f, %.1f, %.1f (nanoseconds).",
            //        stat_n,return_time,stat_mean, sqrtf(stat_M2 / (stat_n - 1)));

            // here, pick the record with the least dispersion, and record that it's been chosen

            // uint64_t local_time_chosen = arrival_time;
            // uint64_t remote_time_chosen = distant_transmit_time;
            // now pick the timestamp with the lowest dispersion
            uint64_t rt = conn->time_pings[0].remote_time;
            uint64_t lt = conn->time_pings[0].local_time;
            uint64_t tld = conn->time_pings[0].dispersion;
            int chosen = 0;
            for (cc = 1; cc < conn->time_ping_count; cc++)
              if (conn->time_pings[cc].dispersion < tld) {
                chosen = cc;
                rt = conn->time_pings[cc].remote_time;
                lt = conn->time_pings[cc].local_time;
                tld = conn->time_pings[cc].dispersion;
                // local_time_chosen = conn->time_pings[cc].local_time;
                // remote_time_chosen = conn->time_pings[cc].remote_time;
              }
            // debug(1,"Record %d has the lowest dispersion with %0.2f us
            // dispersion.",chosen,1.0*((tld * 1000000) >> 32));
            conn->time_pings[chosen].chosen = 1; // record the fact that it has been used for timing

            conn->local_to_remote_time_difference =
                rt - lt; // make this the new local-to-remote-time-difference
            conn->local_to_remote_time_difference_measurement_time = lt; // done at this time.

            if (first_local_to_remote_time_difference == 0) {
              first_local_to_remote_time_difference = conn->local_to_remote_time_difference;
              // first_local_to_remote_time_difference_time = get_absolute_time_in_fp();
            }

            // here, let's try to use the timing pings that were selected because of their short
            // return times to
            // estimate a figure for drift between the local clock (x) and the remote clock (y)

            // if we plug in a local interval, we will get back what that is in remote time

            // calculate the line of best fit for relating the local time and the remote time
            // we will calculate the slope, which is the drift
            // see https://www.varsitytutors.com/hotmath/hotmath_help/topics/line-of-best-fit

            uint64_t y_bar = 0; // remote timestamp average
            uint64_t x_bar = 0; // local timestamp average
            int sample_count = 0;

            // approximate time in seconds to let the system settle down
            const int settling_time = 60;
            // number of points to have for calculating a valid drift
            const int sample_point_minimum = 8;
            for (cc = 0; cc < conn->time_ping_count; cc++)
              if ((conn->time_pings[cc].chosen) &&
                  (conn->time_pings[cc].sequence_number >
                   (settling_time / 3))) { // wait for a approximate settling time
                                           // have to scale them down so that the sum, possibly
                                           // over every term in the array, doesn't overflow
                y_bar += (conn->time_pings[cc].remote_time >> time_ping_history_power_of_two);
                x_bar += (conn->time_pings[cc].local_time >> time_ping_history_power_of_two);
                sample_count++;
              }
            conn->local_to_remote_time_gradient_sample_count = sample_count;
            if (sample_count > sample_point_minimum) {
              y_bar = y_bar / sample_count;
              x_bar = x_bar / sample_count;

              int64_t xid, yid;
              double mtl, mbl;
              mtl = 0;
              mbl = 0;
              for (cc = 0; cc < conn->time_ping_count; cc++)
                if ((conn->time_pings[cc].chosen) &&
                    (conn->time_pings[cc].sequence_number > (settling_time / 3))) {

                  uint64_t slt = conn->time_pings[cc].local_time >> time_ping_history_power_of_two;
                  if (slt > x_bar)
                    xid = slt - x_bar;
                  else
                    xid = -(x_bar - slt);

                  uint64_t srt = conn->time_pings[cc].remote_time >> time_ping_history_power_of_two;
                  if (srt > y_bar)
                    yid = srt - y_bar;
                  else
                    yid = -(y_bar - srt);

                  mtl = mtl + (1.0 * xid) * yid;
                  mbl = mbl + (1.0 * xid) * xid;
                }
              if (mbl)
                conn->local_to_remote_time_gradient = mtl / mbl;
              else {
                // conn->local_to_remote_time_gradient = 1.0;
                debug(1, "mbl is zero. Drift remains at %.2f ppm.",
                      (conn->local_to_remote_time_gradient - 1.0) * 1000000);
              }

              // scale the numbers back up
              uint64_t ybf = y_bar << time_ping_history_power_of_two;
              uint64_t xbf = x_bar << time_ping_history_power_of_two;

              conn->local_to_remote_time_difference =
                  ybf - xbf; // make this the new local-to-remote-time-difference
              conn->local_to_remote_time_difference_measurement_time = xbf;

            } else {
              debug(3, "not enough samples to estimate drift -- remaining at %.2f ppm.",
                    (conn->local_to_remote_time_gradient - 1.0) * 1000000);
              // conn->local_to_remote_time_gradient = 1.0;
            }
            // debug(1,"local to remote time gradient is %12.2f ppm, based on %d
            // samples.",conn->local_to_remote_time_gradient*1000000,sample_count);
            // debug(1,"ntp set offset and measurement time"); // iin PTP terms, this is the
            // local-to-network offset and the local measurement time
          } else {
            debug(1,
                  "Time ping turnaround time: %" PRIu64
                  " ns -- it looks like a timing ping was lost.",
                  return_time);
          }
        } else {
          debug(1, "Timing port -- Unknown RTP packet of type 0x%02X length %d.", packet[1], nread);
        }
      } else {
        debug(3, "Timing Receiver Thread -- dropping incoming packet to simulate a bad network.");
      }
    } else {
      debug(1, "Timing receiver -- error receiving a packet.");
    }
  }

  debug(1, "Timing Receiver RTP thread \"normal\" exit -- this can't happen. Hah!");
  pthread_cleanup_pop(0); // don't execute anything here.
  debug(2, "Timing Receiver RTP thread exit.");
  pthread_exit(NULL);
}

void rtp_setup(SOCKADDR *local, SOCKADDR *remote, uint16_t cport, uint16_t tport,
               rtsp_conn_info *conn) {

  // this gets the local and remote ip numbers (and ports used for the TCD stuff)
  // we use the local stuff to specify the address we are coming from and
  // we use the remote stuff to specify where we're goint to

  if (conn->rtp_running)
    warn("rtp_setup has been called with al already-active stream -- ignored. Possible duplicate "
         "SETUP call?");
  else {

    debug(3, "rtp_setup: cport=%d tport=%d.", cport, tport);

    // print out what we know about the client
    void *client_addr = NULL, *self_addr = NULL;
    // int client_port, self_port;
    // char client_port_str[64];
    // char self_addr_str[64];

    conn->connection_ip_family =
        remote->SAFAMILY; // keep information about the kind of ip of the client

#ifdef AF_INET6
    if (conn->connection_ip_family == AF_INET6) {
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)remote;
      client_addr = &(sa6->sin6_addr);
      // client_port = ntohs(sa6->sin6_port);
      sa6 = (struct sockaddr_in6 *)local;
      self_addr = &(sa6->sin6_addr);
      // self_port = ntohs(sa6->sin6_port);
      conn->self_scope_id = sa6->sin6_scope_id;
    }
#endif
    if (conn->connection_ip_family == AF_INET) {
      struct sockaddr_in *sa4 = (struct sockaddr_in *)remote;
      client_addr = &(sa4->sin_addr);
      // client_port = ntohs(sa4->sin_port);
      sa4 = (struct sockaddr_in *)local;
      self_addr = &(sa4->sin_addr);
      // self_port = ntohs(sa4->sin_port);
    }

    inet_ntop(conn->connection_ip_family, client_addr, conn->client_ip_string,
              sizeof(conn->client_ip_string));
    inet_ntop(conn->connection_ip_family, self_addr, conn->self_ip_string,
              sizeof(conn->self_ip_string));

    debug(2, "Connection %d: SETUP -- Connection from %s to self at %s.", conn->connection_number,
          conn->client_ip_string, conn->self_ip_string);

    // set up a the record of the remote's control socket
    struct addrinfo hints;
    struct addrinfo *servinfo;

    memset(&conn->rtp_client_control_socket, 0, sizeof(conn->rtp_client_control_socket));
    memset(&hints, 0, sizeof hints);
    hints.ai_family = conn->connection_ip_family;
    hints.ai_socktype = SOCK_DGRAM;
    char portstr[20];
    snprintf(portstr, 20, "%d", cport);
    if (getaddrinfo(conn->client_ip_string, portstr, &hints, &servinfo) != 0)
      die("Can't get address of client's control port");

#ifdef AF_INET6
    if (servinfo->ai_family == AF_INET6) {
      memcpy(&conn->rtp_client_control_socket, servinfo->ai_addr, sizeof(struct sockaddr_in6));
      // ensure the scope id matches that of remote. this is needed for link-local addresses.
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&conn->rtp_client_control_socket;
      sa6->sin6_scope_id = conn->self_scope_id;
    } else
#endif
      memcpy(&conn->rtp_client_control_socket, servinfo->ai_addr, sizeof(struct sockaddr_in));
    freeaddrinfo(servinfo);

    // set up a the record of the remote's timing socket
    memset(&conn->rtp_client_timing_socket, 0, sizeof(conn->rtp_client_timing_socket));
    memset(&hints, 0, sizeof hints);
    hints.ai_family = conn->connection_ip_family;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portstr, 20, "%d", tport);
    if (getaddrinfo(conn->client_ip_string, portstr, &hints, &servinfo) != 0)
      die("Can't get address of client's timing port");
#ifdef AF_INET6
    if (servinfo->ai_family == AF_INET6) {
      memcpy(&conn->rtp_client_timing_socket, servinfo->ai_addr, sizeof(struct sockaddr_in6));
      // ensure the scope id matches that of remote. this is needed for link-local addresses.
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&conn->rtp_client_timing_socket;
      sa6->sin6_scope_id = conn->self_scope_id;
    } else
#endif
      memcpy(&conn->rtp_client_timing_socket, servinfo->ai_addr, sizeof(struct sockaddr_in));
    freeaddrinfo(servinfo);

    // now, we open three sockets -- one for the audio stream, one for the timing and one for the
    // control
    conn->remote_control_port = cport;
    conn->remote_timing_port = tport;

    conn->local_control_port = bind_UDP_port(conn->connection_ip_family, conn->self_ip_string,
                                             conn->self_scope_id, &conn->control_socket);
    conn->local_timing_port = bind_UDP_port(conn->connection_ip_family, conn->self_ip_string,
                                            conn->self_scope_id, &conn->timing_socket);
    conn->local_audio_port = bind_UDP_port(conn->connection_ip_family, conn->self_ip_string,
                                           conn->self_scope_id, &conn->audio_socket);

    debug(3, "listening for audio, control and timing on ports %d, %d, %d.", conn->local_audio_port,
          conn->local_control_port, conn->local_timing_port);

    conn->anchor_rtptime = 0;

    conn->request_sent = 0;
    conn->rtp_running = 1;
  }
}

void reset_ntp_anchor_info(rtsp_conn_info *conn) {
  debug_mutex_lock(&conn->reference_time_mutex, 1000, 1);
  conn->anchor_remote_info_is_valid = 0;
  conn->anchor_rtptime = 0;
  conn->anchor_time = 0;
  debug_mutex_unlock(&conn->reference_time_mutex, 3);
}

int have_ntp_timing_information(rtsp_conn_info *conn) {
  if (conn->anchor_remote_info_is_valid != 0)
    return 1;
  else
    return 0;
}

// the timestamp is a timestamp calculated at the input rate
// the reference timestamps are denominated in terms of the input rate

int frame_to_ntp_local_time(uint32_t timestamp, uint64_t *time, rtsp_conn_info *conn) {
  // a zero result is good
  if (conn->anchor_remote_info_is_valid == 0)
    debug(1, "no anchor information");
  debug_mutex_lock(&conn->reference_time_mutex, 1000, 0);
  int result = -1;
  if (conn->anchor_remote_info_is_valid != 0) {
    uint64_t remote_time_of_timestamp;
    int32_t timestamp_interval = timestamp - conn->anchor_rtptime;
    int64_t timestamp_interval_time = timestamp_interval;
    timestamp_interval_time = timestamp_interval_time * 1000000000;
    timestamp_interval_time =
        timestamp_interval_time / conn->input_rate; // this is the nominal time, based on the
                                                    // fps specified between current and
                                                    // previous sync frame.
    remote_time_of_timestamp =
        conn->anchor_time + timestamp_interval_time; // based on the reference timestamp time
                                                     // plus the time interval calculated based
                                                     // on the specified fps.
    if (time != NULL)
      *time = remote_time_of_timestamp - local_to_remote_time_difference_now(conn);
    result = 0;
  }
  debug_mutex_unlock(&conn->reference_time_mutex, 0);
  return result;
}

int local_ntp_time_to_frame(uint64_t time, uint32_t *frame, rtsp_conn_info *conn) {
  // a zero result is good
  debug_mutex_lock(&conn->reference_time_mutex, 1000, 0);
  int result = -1;
  if (conn->anchor_remote_info_is_valid != 0) {
    // first, get from [local] time to remote time.
    uint64_t remote_time = time + local_to_remote_time_difference_now(conn);
    // next, get the remote time interval from the remote_time to the reference time
    // here, we calculate the time interval, in terms of remote time
    int64_t offset = remote_time - conn->anchor_time;
    // now, convert the remote time interval into frames using the frame rate we have observed or
    // which has been nominated
    int64_t frame_interval = 0;
    frame_interval = (offset * conn->input_rate) / 1000000000;
    int32_t frame_interval_32 = frame_interval;
    uint32_t new_frame = conn->anchor_rtptime + frame_interval_32;
    // debug(1,"frame is %u.", new_frame);
    if (frame != NULL)
      *frame = new_frame;
    result = 0;
  }
  debug_mutex_unlock(&conn->reference_time_mutex, 0);
  return result;
}

void rtp_request_resend(seq_t first, uint32_t count, rtsp_conn_info *conn) {
  // debug(1, "rtp_request_resend of %u packets from sequence number %u.", count, first);
  if (conn->rtp_running) {
    // if (!request_sent) {
    // debug(2, "requesting resend of %d packets starting at %u.", count, first);
    //  request_sent = 1;
    //}

    char req[8]; // *not* a standard RTCP NACK
    req[0] = 0x80;
#ifdef CONFIG_AIRPLAY_2
    if (conn->airplay_type == ap_2) {
      if (conn->ap2_remote_control_socket_addr_length == 0) {
        debug(2, "No remote socket -- skipping the resend");
        return; // hack
      }
      req[1] = 0xD5; // Airplay 2 'resend'
    } else {
#endif
      req[1] = (char)0x55 | (char)0x80; // Apple 'resend'
#ifdef CONFIG_AIRPLAY_2
    }
#endif
    *(unsigned short *)(req + 2) = htons(1);     // our sequence number
    *(unsigned short *)(req + 4) = htons(first); // missed seqnum
    *(unsigned short *)(req + 6) = htons(count); // count

    uint64_t time_of_sending_ns = get_absolute_time_in_ns();
    uint64_t resend_error_backoff_time = 300000000; // 0.3 seconds
    if ((conn->rtp_time_of_last_resend_request_error_ns == 0) ||
        ((time_of_sending_ns - conn->rtp_time_of_last_resend_request_error_ns) >
         resend_error_backoff_time)) {
      if ((config.diagnostic_drop_packet_fraction == 0.0) ||
          (drand48() > config.diagnostic_drop_packet_fraction)) {
        // put a time limit on the sendto

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        int response;
#ifdef CONFIG_AIRPLAY_2
        if (conn->airplay_type == ap_2) {
          if (setsockopt(conn->ap2_control_socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout,
                         sizeof(timeout)) < 0)
            debug(1, "Can't set timeout on resend request socket.");
          response = sendto(conn->ap2_control_socket, req, sizeof(req), 0,
                            (struct sockaddr *)&conn->ap2_remote_control_socket_addr,
                            conn->ap2_remote_control_socket_addr_length);
        } else {
#endif
          if (setsockopt(conn->control_socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout,
                         sizeof(timeout)) < 0)
            debug(1, "Can't set timeout on resend request socket.");
          socklen_t msgsize = sizeof(struct sockaddr_in);
#ifdef AF_INET6
          if (conn->rtp_client_control_socket.SAFAMILY == AF_INET6) {
            msgsize = sizeof(struct sockaddr_in6);
          }
#endif
          response = sendto(conn->control_socket, req, sizeof(req), 0,
                            (struct sockaddr *)&conn->rtp_client_control_socket, msgsize);

#ifdef CONFIG_AIRPLAY_2
        }
#endif
        if (response == -1) {
          char em[1024];
          strerror_r(errno, em, sizeof(em));
          debug(2, "Error %d using sendto to request a resend: \"%s\".", errno, em);
          conn->rtp_time_of_last_resend_request_error_ns = time_of_sending_ns;
        } else {
          conn->rtp_time_of_last_resend_request_error_ns = 0;
        }

      } else {
        debug(3, "Dropping resend request packet to simulate a bad network. Backing off for 0.3 "
                 "second.");
        conn->rtp_time_of_last_resend_request_error_ns = time_of_sending_ns;
      }
    } else {
      debug(1,
            "Suppressing a resend request due to a resend sendto error in the last 0.3 seconds.");
    }
  } else {
    // if (!request_sent) {
    debug(2, "rtp_request_resend called without active stream!");
    //  request_sent = 1;
    //}
  }
}

#ifdef CONFIG_AIRPLAY_2

void set_ptp_anchor_info(rtsp_conn_info *conn, uint64_t clock_id, uint32_t rtptime,
                         uint64_t networktime) {
  if ((conn->anchor_clock != 0) && (conn->anchor_clock == clock_id) &&
      (conn->anchor_remote_info_is_valid != 0)) {
    // check change in timing
    int64_t time_difference = networktime - conn->anchor_time;
    int32_t frame_difference = rtptime - conn->anchor_rtptime;
    double time_difference_in_frames = (1.0 * time_difference * conn->input_rate) / 1000000000;
    double frame_change = frame_difference - time_difference_in_frames;
    debug(3,
          "Connection %d: set_ptp_anchor_info: clock: %" PRIx64 ", rtptime: %" PRIu32
          ", networktime: %" PRIx64 ", frame adjustment: %7.3f.",
          conn->connection_number, clock_id, rtptime, networktime, frame_change);
  } else {
    debug(2,
          "Connection %d: set_ptp_anchor_info: clock: %" PRIx64 ", rtptime: %" PRIu32
          ", networktime: %" PRIx64 ".",
          conn->connection_number, clock_id, rtptime, networktime);
  }
  if (conn->anchor_clock != clock_id) {
    debug(2, "Connection %d: Set Anchor Clock: %" PRIx64 ".", conn->connection_number, clock_id);
  }
  // debug(1,"set anchor info clock: %" PRIx64", rtptime: %u, networktime: %" PRIx64 ".", clock_id,
  // rtptime, networktime);

  // if the clock is the same but any details change, and if the last_anchor_info has not been
  // valid for some minimum time (and thus may not be reliable), we need to invalidate
  // last_anchor_info

  if ((conn->airplay_stream_type == buffered_stream) && (conn->ap2_play_enabled != 0) &&
      ((clock_id != conn->anchor_clock) || (conn->anchor_rtptime != rtptime) ||
       (conn->anchor_time != networktime))) {
    uint64_t master_clock_id = 0;
    ptp_get_clock_info(&master_clock_id, NULL, NULL, NULL);
    debug(1,
          "Connection %d: Note: anchor parameters have changed. Old clock: %" PRIx64
          ", rtptime: %u, networktime: %" PRIu64 ". New clock: %" PRIx64
          ", rtptime: %u, networktime: %" PRIu64 ". Current master clock: %" PRIx64 ".",
          conn->connection_number, conn->anchor_clock, conn->anchor_rtptime, conn->anchor_time,
          clock_id, rtptime, networktime, master_clock_id);
  }

  if ((clock_id == conn->anchor_clock) &&
      ((conn->anchor_rtptime != rtptime) || (conn->anchor_time != networktime))) {
    uint64_t time_now = get_absolute_time_in_ns();
    int64_t last_anchor_validity_duration = time_now - conn->last_anchor_validity_start_time;
    if (last_anchor_validity_duration < 5000000000) {
      if (conn->airplay_stream_type == buffered_stream)
        debug(2,
              "Connection %d: Note: anchor parameters have changed before clock %" PRIx64
              " has stabilised.",
              conn->connection_number, clock_id);
      conn->last_anchor_info_is_valid = 0;
    }
  }

  conn->anchor_remote_info_is_valid = 1;

  // these can be modified if the master clock changes over time
  conn->anchor_rtptime = rtptime;
  conn->anchor_time = networktime;
  conn->anchor_clock = clock_id;
}
int long_time_notifcation_done = 0;

uint64_t previous_offset = 0;
uint64_t previous_clock_id = 0;

void reset_ptp_anchor_info(rtsp_conn_info *conn) {
  debug(2, "Connection %d: Clear anchor information.", conn->connection_number);
  conn->last_anchor_info_is_valid = 0;
  conn->anchor_remote_info_is_valid = 0;
  long_time_notifcation_done = 0;
  previous_offset = 0;
  previous_clock_id = 0;
}

int get_ptp_anchor_local_time_info(rtsp_conn_info *conn, uint32_t *anchorRTP,
                                   uint64_t *anchorLocalTime) {
  int response = clock_no_anchor_info; // no anchor information
  if (conn->anchor_remote_info_is_valid != 0) {
    response = clock_not_valid;
    uint64_t actual_clock_id;
    uint64_t actual_time_of_sample, actual_offset, start_of_mastership;
    response = ptp_get_clock_info(&actual_clock_id, &actual_time_of_sample, &actual_offset,
                                  &start_of_mastership);
    if (response == clock_ok) {
      uint64_t time_now = get_absolute_time_in_ns();
      int64_t time_since_start_of_mastership = time_now - start_of_mastership;
      if (time_since_start_of_mastership >= 400000000L) {
        int64_t time_since_sample = time_now - actual_time_of_sample;
        if (time_since_sample > 300000000000L) {
          if (long_time_notifcation_done == 0) {
            debug(1, "The last PTP timing sample is pretty old: %f seconds.",
                  0.000000001 * time_since_sample);
            long_time_notifcation_done = 1;
          }
        } else if ((time_since_sample < 2000000000) && (long_time_notifcation_done != 0)) {
          debug(1, "The last PTP timing sample is no longer too old: %f seconds.",
                0.000000001 * time_since_sample);
          long_time_notifcation_done = 0;
        }

        int64_t jitter = actual_offset - previous_offset;

        if ((previous_offset != 0) && (previous_clock_id == actual_clock_id) &&
            ((jitter > 3000000) || (jitter < -3000000)))
          debug(1,
                "Clock jitter: %" PRId64 " ns. Time since sample: %" PRId64
                " ns. Time since start of mastership: %f "
                "seconds.",
                jitter, time_since_sample, time_since_start_of_mastership * 0.000000001);

        previous_offset = actual_offset;
        previous_clock_id = actual_clock_id;

        if (actual_clock_id == conn->anchor_clock) {
          conn->last_anchor_rtptime = conn->anchor_rtptime;
          conn->last_anchor_local_time = conn->anchor_time - actual_offset;
          conn->last_anchor_time_of_update = time_now;
          if (conn->last_anchor_info_is_valid == 0)
            conn->last_anchor_validity_start_time = start_of_mastership;
          conn->last_anchor_info_is_valid = 1;
        } else {
          debug(3, "Current master clock %" PRIx64 " and anchor_clock %" PRIx64 " are different",
                actual_clock_id, conn->anchor_clock);
          // the anchor clock and the actual clock are different

          if (conn->last_anchor_info_is_valid != 0) {

            int64_t time_since_last_update =
                get_absolute_time_in_ns() - conn->last_anchor_time_of_update;
            if (time_since_last_update > 5000000000) {
              int64_t duration_of_mastership = time_now - start_of_mastership;
              debug(2,
                    "Connection %d: Master clock has changed to %" PRIx64
                    ". History: %.3f milliseconds.",
                    conn->connection_number, actual_clock_id, 0.000001 * duration_of_mastership);

              // Now, the thing is that while the anchor clock and master clock for a
              // buffered session start off the same,
              // the master clock can change without the anchor clock changing.
              // SPS gives the new master clock time to settle down and then
              // calculates the appropriate offset to it by
              // calculating back from the local anchor information and the new clock's
              // advertised offset.

              conn->anchor_time = conn->last_anchor_local_time + actual_offset;
              conn->anchor_clock = actual_clock_id;
            }

          } else {
            response = clock_not_valid; // no current clock information and no previous clock info
          }
        }

      } else {
        // debug(1, "mastership time: %f s.", time_since_start_of_mastership * 0.000000001);
        response = clock_not_valid; // hasn't been master for long enough...
      }
    }

    // here, check and update the clock status
    if ((clock_status_t)response != conn->clock_status) {
      switch (response) {
      case clock_ok:
        debug(2, "Connection %d: NQPTP master clock %" PRIx64 ".", conn->connection_number,
              actual_clock_id);
        break;
      case clock_not_ready:
        debug(2, "Connection %d: NQPTP master clock %" PRIx64 " is available but not ready.",
              conn->connection_number, actual_clock_id);
        break;
      case clock_service_unavailable:
        debug(1, "Connection %d: NQPTP clock is not available.", conn->connection_number);
        warn("Can't access the NQPTP clock. Is NQPTP running?");
        break;
      case clock_access_error:
        debug(2, "Connection %d: Error accessing the NQPTP clock interface.",
              conn->connection_number);
        break;
      case clock_data_unavailable:
        debug(1, "Connection %d: Can not access NQPTP clock information.", conn->connection_number);
        break;
      case clock_no_master:
        debug(2, "Connection %d: No NQPTP master clock.", conn->connection_number);
        break;
      case clock_no_anchor_info:
        debug(2, "Connection %d: Awaiting clock anchor information.", conn->connection_number);
        break;
      case clock_version_mismatch:
        debug(2, "Connection %d: NQPTP clock interface mismatch.", conn->connection_number);
        warn(
            "This version of Shairport Sync is not compatible with the installed version of NQPTP. "
            "Please update.");
        break;
      case clock_not_synchronised:
        debug(1, "Connection %d: NQPTP clock is not synchronised.", conn->connection_number);
        break;
      case clock_not_valid:
        debug(2, "Connection %d: NQPTP clock information is not valid.", conn->connection_number);
        break;
      default:
        debug(1, "Connection %d: NQPTP clock reports an unrecognised status: %u.",
              conn->connection_number, response);
        break;
      }
      conn->clock_status = response;
    }

    if (conn->last_anchor_info_is_valid != 0) {
      if (anchorRTP != NULL)
        *anchorRTP = conn->last_anchor_rtptime;
      if (anchorLocalTime != NULL)
        *anchorLocalTime = conn->last_anchor_local_time;
    }
  }
  return response;
}

int have_ptp_timing_information(rtsp_conn_info *conn) {
  if (get_ptp_anchor_local_time_info(conn, NULL, NULL) == clock_ok)
    return 1;
  else
    return 0;
}

int frame_to_ptp_local_time(uint32_t timestamp, uint64_t *time, rtsp_conn_info *conn) {
  int result = -1;
  uint32_t anchor_rtptime = 0;
  uint64_t anchor_local_time = 0;
  if (get_ptp_anchor_local_time_info(conn, &anchor_rtptime, &anchor_local_time) == clock_ok) {
    int32_t frame_difference = timestamp - anchor_rtptime;
    int64_t time_difference = frame_difference;
    time_difference = time_difference * 1000000000;
    if (conn->input_rate == 0)
      die("conn->input_rate is zero!");
    time_difference = time_difference / conn->input_rate;
    uint64_t ltime = anchor_local_time + time_difference;
    *time = ltime;
    result = 0;
  } else {
    debug(3, "frame_to_ptp_local_time can't get anchor local time information");
  }
  return result;
}

int local_ptp_time_to_frame(uint64_t time, uint32_t *frame, rtsp_conn_info *conn) {
  int result = -1;
  uint32_t anchor_rtptime = 0;
  uint64_t anchor_local_time = 0;
  if (get_ptp_anchor_local_time_info(conn, &anchor_rtptime, &anchor_local_time) == clock_ok) {
    int64_t time_difference = time - anchor_local_time;
    int64_t frame_difference = time_difference;
    frame_difference = frame_difference * conn->input_rate; // but this is by 10^9
    frame_difference = frame_difference / 1000000000;
    int32_t fd32 = frame_difference;
    uint32_t lframe = anchor_rtptime + fd32;
    *frame = lframe;
    result = 0;
  } else {
    debug(3, "local_ptp_time_to_frame can't get anchor local time information");
  }
  return result;
}

void rtp_event_receiver_cleanup_handler(void *arg) {
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  debug(2, "Connection %d: AP2 Event Receiver Cleanup.", conn->connection_number);
}

void *rtp_event_receiver(void *arg) {
  //  #include <syscall.h>
  //  debug(1, "rtp_event_receiver PID %d", syscall(SYS_gettid));
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  if (conn->airplay_stream_category == remote_control_stream)
    debug(2, "Connection %d (RC): AP2 Event Receiver started", conn->connection_number);
  else
    debug(2, "Connection %d: AP2 Event Receiver started", conn->connection_number);

  structured_buffer *sbuf = sbuf_new(4096);
  if (sbuf != NULL) {
    pthread_cleanup_push(sbuf_cleanup, sbuf);

    pthread_cleanup_push(rtp_event_receiver_cleanup_handler, arg);

    // listen(conn->event_socket, 5); // this is now done in the handle_setup_2 code

    uint8_t packet[4096];
    ssize_t nread;
    SOCKADDR remote_addr;
    memset(&remote_addr, 0, sizeof(remote_addr));
    socklen_t addr_size = sizeof(remote_addr);

    int fd = accept(conn->event_socket, (struct sockaddr *)&remote_addr, &addr_size);
    debug(2,
          "Connection %d: rtp_event_receiver accepted a connection on socket %d and moved to a new "
          "socket %d.",
          conn->connection_number, conn->event_socket, fd);
    intptr_t pfd = fd;
    pthread_cleanup_push(socket_cleanup, (void *)pfd);
    int finished = 0;
    do {

      plist_t value_plist = generateInfoPlist(conn);
      if (value_plist != NULL) {
        void *txtData = NULL;
        size_t txtDataLength = 0;
        generateTxtDataValueInfo(conn, &txtData, &txtDataLength);
        plist_dict_set_item(value_plist, "txtAirPlay", plist_new_data(txtData, txtDataLength));
        free(txtData);
        plist_t update_info_plist = plist_new_dict();
        if (update_info_plist != NULL) {
          plist_dict_set_item(update_info_plist, "type", plist_new_string("updateInfo"));
          plist_dict_set_item(update_info_plist, "value", value_plist);
          char *plistString = NULL;
          uint32_t plistStringLength = 0;
          plist_to_bin(update_info_plist, &plistString, &plistStringLength);
          if (plistString != NULL) {
            char *plist_as_string = plist_as_xml_text(update_info_plist);
            if (plist_as_string != NULL) {
              debug(3, "Plist is: \"%s\".", plist_as_string);
              free(plist_as_string);
            }
            sbuf_printf(sbuf, "POST /command RTSP/1.0\r\nContent-Length: %u\r\n",
                        plistStringLength);
            sbuf_printf(sbuf, "Content-Type: application/x-apple-binary-plist\r\n\r\n");
            sbuf_append(sbuf, plistString, plistStringLength);

            free(plistString); // should be plist_to_bin_free, but it's not defined in older
                               // libraries
            char *b = 0;
            size_t l = 0;
            sbuf_buf_and_length(sbuf, &b, &l);
            ssize_t wres =
                write_encrypted(fd, &conn->ap2_pairing_context.event_cipher_bundle, b, l);
            if ((wres == -1) || ((size_t)wres != l))
              debug(1, "Encrypted write error");

            sbuf_clear(sbuf);
          } else {
            debug(1, "plist string not created!");
          }
          plist_free(update_info_plist);
        } else {
          debug(1, "Could not build an updateInfo plist");
        }
        // plist_free(value_plist);
      } else {
        debug(1, "Could not build an value plist");
      }

      while (finished == 0) {
        nread = read_encrypted(fd, &conn->ap2_pairing_context.event_cipher_bundle, packet,
                               sizeof(packet));

        // nread = recv(fd, packet, sizeof(packet), 0);

        if (nread < 0) {
          char errorstring[1024];
          strerror_r(errno, (char *)errorstring, sizeof(errorstring));
          debug(
              1,
              "Connection %d: error in ap2 rtp_event_receiver %d: \"%s\". Could not recv a packet.",
              conn->connection_number, errno, errorstring);
          // if ((config.diagnostic_drop_packet_fraction == 0.0) ||
          //     (drand48() > config.diagnostic_drop_packet_fraction)) {
        } else if (nread > 0) {

          // ssize_t plen = nread;
          packet[nread] = '\0';
          debug(3, "Connection %d: Packet Received on Event Port with contents: \"%s\".",
                conn->connection_number, packet);
        } else {
          debug(2, "Connection %d: Event Port connection closed by client",
                conn->connection_number);
          finished = 1;
        }
      }

    } while (finished == 0);
    pthread_cleanup_pop(1); // close the socket
    pthread_cleanup_pop(1); // do the cleanup
    pthread_cleanup_pop(1); // delete the structured buffer
    debug(2, "Connection %d: AP2 Event Receiver RTP thread \"normal\" exit.",
          conn->connection_number);
  } else {
    debug(1, "Could not allocate a structured buffer!");
  }
  pthread_exit(NULL);
}

void rtp_ap2_control_handler_cleanup_handler(void *arg) {
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  debug(2, "Connection %d: AP2 Control Receiver Cleanup.", conn->connection_number);
  close(conn->ap2_control_socket);
  debug(2, "Connection %d: UDP control port %u closed.", conn->connection_number,
        conn->local_ap2_control_port);
  conn->ap2_control_socket = 0;
  conn->ap2_remote_control_socket_addr_length =
      0; // indicates to the control receiver thread that the socket address need to be
         // recreated (needed for resend requests in the realtime mode)
}

int32_t decipher_player_put_packet(uint8_t *ciphered_audio_alt, ssize_t nread,
                                   rtsp_conn_info *conn) {

  // this deciphers the packet -- it doesn't decode it from ALAC
  uint16_t sequence_number = 0;

  // if the packet is too small, don't go ahead.
  // it must contain an uint16_t sequence number and eight bytes of AAD followed by the
  // ciphertext and then followed by an eight-byte nonce. Thus it must be greater than 18
  if (nread > 18) {

    memcpy(&sequence_number, ciphered_audio_alt, sizeof(uint16_t));
    sequence_number = ntohs(sequence_number);

    uint32_t timestamp;
    memcpy(&timestamp, ciphered_audio_alt + sizeof(uint16_t), sizeof(uint32_t));
    timestamp = ntohl(timestamp);

    if (conn->session_key != NULL) {
      unsigned char nonce[12];
      memset(nonce, 0, sizeof(nonce));
      memcpy(nonce + 4, ciphered_audio_alt + nread - 8,
             8); // front-pad the 8-byte nonce received to get the 12-byte nonce expected

      // https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305/ietf_chacha20-poly1305_construction
      // Note: the eight-byte nonce must be front-padded out to 12 bytes.

      unsigned char m[4096];
      unsigned long long new_payload_length = 0;
      int response = crypto_aead_chacha20poly1305_ietf_decrypt(
          m,                   // m
          &new_payload_length, // mlen_p
          NULL,                // nsec,
          ciphered_audio_alt +
              10,           // the ciphertext starts 10 bytes in and is followed by the MAC tag,
          nread - (8 + 10), // clen -- the last 8 bytes are the nonce
          ciphered_audio_alt + 2, // authenticated additional data
          8,                      // authenticated additional data length
          nonce,
          conn->session_key); // *k
      if (response != 0) {
        debug(1, "Error decrypting an audio packet.");
      }
      // now pass it in to the regular processing chain

      unsigned long long max_int = INT_MAX; // put in the right format
      if (new_payload_length > max_int)
        debug(1, "Madly long payload length!");
      int plen = new_payload_length; //
      // debug(1,"                                                        Write packet to buffer %d,
      // timestamp %u.", sequence_number, timestamp);
      player_put_packet(ALAC_44100_S16_2, sequence_number, timestamp, m, plen, 0, 0,
                        conn); // 0 = no mute, 0 = non discontinuous
    } else {
      debug(2, "No session key, so the audio packet can not be deciphered -- skipped.");
    }
    return sequence_number;
  } else {
    debug(1, "packet was too small -- ignored");
    return -1;
  }
}

void *rtp_ap2_control_receiver(void *arg) {
  //  #include <syscall.h>
  //  debug(1, "rtp_ap2_control_receiver PID %d", syscall(SYS_gettid));
  pthread_cleanup_push(rtp_ap2_control_handler_cleanup_handler, arg);
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  uint8_t packet[4096];
  ssize_t nread;
  int keep_going = 1;
  uint64_t start_time = get_absolute_time_in_ns();
  uint64_t packet_number = 0;
  while (keep_going) {
    SOCKADDR from_sock_addr;
    socklen_t from_sock_addr_length = sizeof(SOCKADDR);
    memset(&from_sock_addr, 0, sizeof(SOCKADDR));

    nread = recvfrom(conn->ap2_control_socket, packet, sizeof(packet), 0,
                     (struct sockaddr *)&from_sock_addr, &from_sock_addr_length);
    uint64_t time_now = get_absolute_time_in_ns();
    int64_t time_since_start = time_now - start_time;

    if (conn->udp_clock_is_initialised == 0) {
      packet_number = 0;
      conn->udp_clock_is_initialised = 1;
      debug(2, "AP2 Realtime Clock receiver initialised.");
    }

    // debug(1,"Connection %d: AP2 Control Packet received.", conn->connection_number);

    if (nread >= 28) { // must have at least 28 bytes for the timing information
      if ((time_since_start < 2000000) && ((packet[0] & 0x10) == 0)) {
        debug(1,
              "Dropping what looks like a (non-sentinel) packet left over from a previous session "
              "at %f ms.",
              0.000001 * time_since_start);
      } else {
        packet_number++;
        // debug(1,"AP2 Packet %" PRIu64 ".", packet_number);

        if (packet_number == 1) {
          if ((packet[0] & 0x10) != 0) {
            debug(2, "First packet is a sentinel packet.");
          } else {
            debug(2, "First packet is a not a sentinel packet!");
          }
        }
        // debug(1,"rtp_ap2_control_receiver coded: %u, %u", packet[0], packet[1]);
        // you might want to set this higher to specify how many initial timings to ignore
        if (packet_number >= 1) {
          if ((config.diagnostic_drop_packet_fraction == 0.0) ||
              (drand48() > config.diagnostic_drop_packet_fraction)) {
            // store the from_sock_addr if we haven't already done so
            // v remember to zero this when you're finished!
            if (conn->ap2_remote_control_socket_addr_length == 0) {
              memcpy(&conn->ap2_remote_control_socket_addr, &from_sock_addr, from_sock_addr_length);
              conn->ap2_remote_control_socket_addr_length = from_sock_addr_length;
            }
            switch (packet[1]) {
            case 215: // code 215, effectively an anchoring announcement
            {
              // struct timespec tnr;
              // clock_gettime(CLOCK_REALTIME, &tnr);
              // uint64_t local_realtime_now = timespec_to_ns(&tnr);

              /*
                        char obf[4096];
                        char *obfp = obf;
                        int obfc;
                        for (obfc=0;obfc<nread;obfc++) {
                          snprintf(obfp, 3, "%02X", packet[obfc]);
                          obfp+=2;
                        };
                        *obfp=0;
                        debug(1,"AP2 Timing Control Received: \"%s\"",obf);
              */

              uint64_t remote_packet_time_ns = nctoh64(packet + 8);
              check64conversion("remote_packet_time_ns", packet + 8, remote_packet_time_ns);
              uint64_t clock_id = nctoh64(packet + 20);
              check64conversion("clock_id", packet + 20, clock_id);

              // debug(1, "we have clock_id: %" PRIx64 ".", clock_id);
              // debug(1,"remote_packet_time_ns: %" PRIx64 ", local_realtime_now_ns: %" PRIx64
              // ".", remote_packet_time_ns, local_realtime_now);
              uint32_t frame_1 =
                  nctohl(packet + 4); // this seems to be the frame with latency of 77165 included
              check32conversion("frame_1", packet + 4, frame_1);
              uint32_t frame_2 =
                  nctohl(packet + 16); // this seems to be the frame the time refers to
              check32conversion("frame_2", packet + 16, frame_2);
              // this just updates the anchor information contained in the packet
              // the frame and its remote time
              // add in the audio_backend_latency_offset;
              int32_t notified_latency = frame_2 - frame_1;
              if (notified_latency != 77175)
                debug(1, "Notified latency is %d frames.", notified_latency);
              int32_t added_latency =
                  (int32_t)(config.audio_backend_latency_offset * conn->input_rate);
              // the actual latency is the notified latency plus the fixed latency + the added
              // latency

              int32_t net_latency =
                  notified_latency + 11035 +
                  added_latency; // this is the latency between incoming frames and the DAC
              net_latency = net_latency - (int32_t)(config.audio_backend_buffer_desired_length *
                                                    conn->input_rate);
              // debug(1, "Net latency is %d frames.", net_latency);

              if (net_latency <= 0) {
                if (conn->latency_warning_issued == 0) {
                  warn("The stream latency (%f seconds) it too short to accommodate an offset of "
                       "%f "
                       "seconds and a backend buffer of %f seconds.",
                       ((notified_latency + 11035) * 1.0) / conn->input_rate,
                       config.audio_backend_latency_offset,
                       config.audio_backend_buffer_desired_length);
                  warn("(FYI the stream latency needed would be %f seconds.)",
                       config.audio_backend_buffer_desired_length -
                           config.audio_backend_latency_offset);
                  conn->latency_warning_issued = 1;
                }
                conn->latency = notified_latency + 11035;
              } else {
                conn->latency = notified_latency + 11035 + added_latency;
              }

              set_ptp_anchor_info(conn, clock_id, frame_1 - 11035 - added_latency,
                                  remote_packet_time_ns);
              if (conn->anchor_clock != clock_id) {
                debug(2, "Connection %d: Change Anchor Clock: %" PRIx64 ".",
                      conn->connection_number, clock_id);
              }

            } break;
            case 0xd6:
              // six bytes in is the sequence number at the start of the encrypted audio packet
              // returns the sequence number but we're not really interested
              decipher_player_put_packet(packet + 6, nread - 6, conn);
              break;
            default: {
              char *packet_in_hex_cstring =
                  debug_malloc_hex_cstring(packet, nread); // remember to free this afterwards
              debug(1,
                    "AP2 Control Receiver Packet of first byte 0x%02X, type 0x%02X length %d "
                    "received: "
                    "\"%s\".",
                    packet[0], packet[1], nread, packet_in_hex_cstring);
              free(packet_in_hex_cstring);
            } break;
            }
          } else {
            debug(1, "AP2 Control Receiver -- dropping a packet.");
          }
        }
      }
    } else {
      if (nread == -1) {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
          if (conn->airplay_stream_type == realtime_stream) {
            debug(1,
                  "Connection %d: no control packets for the last 7 seconds -- resetting anchor "
                  "info",
                  conn->connection_number);
            reset_ptp_anchor_info(conn);
            packet_number = 0; // start over in allowing the packet to set anchor information
          }
        } else {
          debug(2, "Connection %d: AP2 Control Receiver -- error %d receiving a packet.",
                conn->connection_number, errno);
        }
      } else {
        debug(2, "Connection %d: AP2 Control Receiver -- malformed packet, %d bytes long.",
              conn->connection_number, nread);
      }
    }
  }
  debug(1, "AP2 Control RTP thread \"normal\" exit -- this can't happen. Hah!");
  pthread_cleanup_pop(1);
  debug(1, "AP2 Control RTP thread exit.");
  pthread_exit(NULL);
}

void rtp_realtime_audio_cleanup_handler(__attribute__((unused)) void *arg) {
  debug(2, "Realtime Audio Receiver Cleanup Start.");
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  close(conn->realtime_audio_socket);
  debug(2, "Connection %d: closing realtime audio port %u", conn->local_realtime_audio_port);
  conn->realtime_audio_socket = 0;
  debug(2, "Realtime Audio Receiver Cleanup Done.");
}

void *rtp_realtime_audio_receiver(void *arg) {
  //  #include <syscall.h>
  //  debug(1, "rtp_realtime_audio_receiver PID %d", syscall(SYS_gettid));
  pthread_cleanup_push(rtp_realtime_audio_cleanup_handler, arg);
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  uint8_t packet[4096];
  int32_t last_seqno = -1;
  ssize_t nread;
  while (1) {
    nread = recv(conn->realtime_audio_socket, packet, sizeof(packet), 0);

    if (nread > 36) { // 36 is the 12-byte header and and 24-byte footer
      if ((config.diagnostic_drop_packet_fraction == 0.0) ||
          (drand48() > config.diagnostic_drop_packet_fraction)) {

        /*
                char *packet_in_hex_cstring =
                    debug_malloc_hex_cstring(packet, nread); // remember to free this afterwards
                debug(1, "Audio Receiver Packet of type 0x%02X length %d received: \"%s\".",
                packet[1], nread, packet_in_hex_cstring);
                free(packet_in_hex_cstring);
        */

        /*
        // debug(1, "Realtime Audio Receiver Packet of type 0x%02X length %d received.", packet[1],
        nread);
        // now get hold of its various bits and pieces
        uint8_t version = (packet[0] & 0b11000000) >> 6;
        uint8_t padding = (packet[0] & 0b00100000) >> 5;
        uint8_t extension = (packet[0] & 0b00010000) >> 4;
        uint8_t csrc_count = packet[0] & 0b00001111;
        uint8_t marker = (packet[1] & 0b1000000) >> 7;
        uint8_t payload_type = packet[1] & 0b01111111;
        */
        // if (have_ptp_timing_information(conn)) {
        if (1) {
          int32_t seqno = decipher_player_put_packet(packet + 2, nread - 2, conn);
          if (seqno >= 0) {
            if (last_seqno == -1) {
              last_seqno = seqno;
            } else {
              last_seqno = (last_seqno + 1) & 0xffff;
              // if (seqno != last_seqno)
              //  debug(3, "RTP: Packets out of sequence: expected: %d, got %d.", last_seqno,
              //  seqno);
              last_seqno = seqno; // reset warning...
            }
          } else {
            debug(1, "Realtime Audio Receiver -- bad packet dropped.");
          }
        }
      } else {
        debug(3, "Realtime Audio Receiver -- dropping a packet.");
      }
    } else {
      debug(1, "Realtime Audio Receiver -- error receiving a packet.");
    }
  }
  pthread_cleanup_pop(0); // don't execute anything here.
  pthread_exit(NULL);
}

ssize_t buffered_read(buffered_tcp_desc *descriptor, void *buf, size_t count,
                      size_t *bytes_remaining) {
  ssize_t response = -1;
  if (debug_mutex_lock(&descriptor->mutex, 50000, 1) != 0)
    debug(1, "problem with mutex");
  pthread_cleanup_push(mutex_unlock, (void *)&descriptor->mutex);
  if (descriptor->closed == 0) {
    if ((descriptor->buffer_occupancy == 0) && (descriptor->error_code == 0)) {
      debug(2, "buffered_read: waiting for %u bytes.", count);
    }
    while ((descriptor->buffer_occupancy == 0) && (descriptor->error_code == 0)) {
      if (pthread_cond_wait(&descriptor->not_empty_cv, &descriptor->mutex))
        debug(1, "Error waiting for buffered read");
      else
        debug(2, "buffered_read: signalled with %u bytes after waiting.",
              descriptor->buffer_occupancy);
    }
  }
  if (descriptor->buffer_occupancy != 0) {
    ssize_t bytes_to_move = count;

    if (descriptor->buffer_occupancy < count) {
      bytes_to_move = descriptor->buffer_occupancy;
    }

    ssize_t top_gap = descriptor->buffer + descriptor->buffer_max_size - descriptor->toq;
    if (top_gap < bytes_to_move)
      bytes_to_move = top_gap;

    memcpy(buf, descriptor->toq, bytes_to_move);
    descriptor->toq += bytes_to_move;
    if (descriptor->toq == descriptor->buffer + descriptor->buffer_max_size)
      descriptor->toq = descriptor->buffer;
    descriptor->buffer_occupancy -= bytes_to_move;
    if (bytes_remaining != NULL)
      *bytes_remaining = descriptor->buffer_occupancy;
    response = bytes_to_move;
    if (pthread_cond_signal(&descriptor->not_full_cv))
      debug(1, "Error signalling");
  } else if (descriptor->error_code) {
    errno = descriptor->error_code;
    response = -1;
  } else if (descriptor->closed != 0) {
    response = 0;
  }

  pthread_cleanup_pop(1); // release the mutex
  return response;
}

#define STANDARD_PACKET_SIZE 4096

void buffered_tcp_reader_cleanup_handler(__attribute__((unused)) void *arg) {
  debug(2, "Buffered TCP Reader Thread Exit via Cleanup.");
}

void *buffered_tcp_reader(void *arg) {
  //  #include <syscall.h>
  //  debug(1, "buffered_tcp_reader PID %d", syscall(SYS_gettid));
  pthread_cleanup_push(buffered_tcp_reader_cleanup_handler, NULL);
  buffered_tcp_desc *descriptor = (buffered_tcp_desc *)arg;

  // listen(descriptor->sock_fd, 5); // this is done in the handle_setup_2 code to ensure it's open
  // when the client hears about it...
  ssize_t nread;
  SOCKADDR remote_addr;
  memset(&remote_addr, 0, sizeof(remote_addr));
  socklen_t addr_size = sizeof(remote_addr);
  int finished = 0;
  int fd = accept(descriptor->sock_fd, (struct sockaddr *)&remote_addr, &addr_size);
  // debug(1, "buffered_tcp_reader: the client has opened a buffered audio link.");
  intptr_t pfd = fd;
  pthread_cleanup_push(socket_cleanup, (void *)pfd);

  do {
    int have_time_to_sleep = 0;
    if (debug_mutex_lock(&descriptor->mutex, 500000, 1) != 0)
      debug(1, "problem with mutex");
    pthread_cleanup_push(mutex_unlock, (void *)&descriptor->mutex);
    while ((descriptor->buffer_occupancy == descriptor->buffer_max_size) ||
           (descriptor->error_code != 0) || (descriptor->closed != 0)) {
      if (pthread_cond_wait(&descriptor->not_full_cv, &descriptor->mutex))
        debug(1, "Error waiting for buffered read");
    }
    pthread_cleanup_pop(1); // release the mutex

    // now we know it is not full, so go ahead and try to read some more into it

    // wrap
    if ((size_t)(descriptor->eoq - descriptor->buffer) == descriptor->buffer_max_size)
      descriptor->eoq = descriptor->buffer;

    // figure out how much to ask for
    size_t bytes_to_request = STANDARD_PACKET_SIZE;
    size_t free_space = descriptor->buffer_max_size - descriptor->buffer_occupancy;
    if (bytes_to_request > free_space)
      bytes_to_request = free_space; // don't ask for more than will fit

    size_t gap_to_end_of_buffer =
        descriptor->buffer + descriptor->buffer_max_size - descriptor->eoq;
    if (gap_to_end_of_buffer < bytes_to_request)
      bytes_to_request =
          gap_to_end_of_buffer; // only ask for what will fill to the top of the buffer

    // do the read
    // debug(1, "Request buffered read  of up to %d bytes.", bytes_to_request);
    nread = recv(fd, descriptor->eoq, bytes_to_request, 0);
    // debug(1, "Received %d bytes for a buffer size of %d bytes.",nread,
    // descriptor->buffer_occupancy + nread);
    if (debug_mutex_lock(&descriptor->mutex, 50000, 1) != 0)
      debug(1, "problem with not empty mutex");
    pthread_cleanup_push(mutex_unlock, (void *)&descriptor->mutex);
    if (nread < 0) {
      char errorstring[1024];
      strerror_r(errno, (char *)errorstring, sizeof(errorstring));
      debug(1, "error in buffered_tcp_reader %d: \"%s\". Could not recv a packet.", errno,
            errorstring);
      descriptor->error_code = errno;
    } else if (nread == 0) {
      descriptor->closed = 1;
    } else if (nread > 0) {
      descriptor->eoq += nread;
      descriptor->buffer_occupancy += nread;
    } else {
      debug(1, "buffered audio port closed!");
    }
    // signal if we got data or an error or the file closed
    if (pthread_cond_signal(&descriptor->not_empty_cv))
      debug(1, "Error signalling");
    if (descriptor->buffer_occupancy > 16384)
      have_time_to_sleep = 1;
    pthread_cleanup_pop(1); // release the mutex
    if (have_time_to_sleep)
      usleep(10000); // give other threads a chance to run...
  } while (finished == 0);

  debug(1, "Buffered TCP Reader Thread Exit \"Normal\" Exit Begin.");
  pthread_cleanup_pop(1); // close the socket
  pthread_cleanup_pop(1); // cleanup
  debug(1, "Buffered TCP Reader Thread Exit \"Normal\" Exit -- Shouldn't happen!.");
  pthread_exit(NULL);
}

// this will read a block of the size specified to the buffer
// and will return either with the block or on error
ssize_t lread_sized_block(buffered_tcp_desc *descriptor, void *buf, size_t count,
                          size_t *bytes_remaining) {
  ssize_t response, nread;
  size_t inbuf = 0; // bytes already in the buffer
  int keep_trying = 1;

  do {
    nread = buffered_read(descriptor, buf + inbuf, count - inbuf, bytes_remaining);
    if (nread == 0) {
      // a blocking read that returns zero means eof -- implies connection closed
      debug(3, "read_sized_block connection closed.");
      keep_trying = 0;
    } else if (nread < 0) {
      if (errno == EAGAIN) {
        debug(1, "read_sized_block getting Error 11 -- EAGAIN from a blocking read!");
      }
      if ((errno != EAGAIN) && (errno != EINTR)) {
        char errorstring[1024];
        strerror_r(errno, (char *)errorstring, sizeof(errorstring));
        debug(1, "read_sized_block read error %d: \"%s\".", errno, (char *)errorstring);
        keep_trying = 0;
      }
    } else {
      inbuf += (size_t)nread;
    }
  } while ((keep_trying != 0) && (inbuf < count));
  if (nread <= 0)
    response = nread;
  else
    response = inbuf;
  return response;
}

// not used right now, but potentially useful for understanding flush requests
void display_flush_requests(int activeOnly, uint32_t currentSeq, uint32_t currentTS,
                            rtsp_conn_info *conn) {
  if (conn->flush_requests == NULL) {
    if (activeOnly == 0)
      debug(1, "No flush requests.");
  } else {
    flush_request_t *t = conn->flush_requests;
    do {
      if (t->flushNow) {
        debug(1, "immediate flush          to untilSeq: %u, untilTS: %u.", t->flushUntilSeq,
              t->flushUntilTS);
      } else {
        if (activeOnly == 0)
          debug(1, "fromSeq: %u, fromTS: %u, to untilSeq: %u, untilTS: %u.", t->flushFromSeq,
                t->flushFromTS, t->flushUntilSeq, t->flushUntilTS);
        else if ((activeOnly == 1) &&
                 (currentSeq >=
                  (t->flushFromSeq -
                   1))) // the -1 is because you might have to trim the end of the previous block
          debug(1,
                "fromSeq: %u, fromTS: %u, to untilSeq: %u, untilTS: %u, with currentSeq: %u, "
                "currentTS: %u.",
                t->flushFromSeq, t->flushFromTS, t->flushUntilSeq, t->flushUntilTS, currentSeq,
                currentTS);
      }
      t = t->next;
    } while (t != NULL);
  }
}

// From
// https://stackoverflow.com/questions/18862715/how-to-generate-the-aac-adts-elementary-stream-with-android-mediacodec
// with thanks!

// See https://wiki.multimedia.cx/index.php/Understanding_AAC
// see also https://wiki.multimedia.cx/index.php/ADTS for the ADTS layout
// see https://wiki.multimedia.cx/index.php/MPEG-4_Audio#Sampling_Frequencies for sampling
// frequencies

/**
 *  Add ADTS header at the beginning of each and every AAC packet.
 *  This is needed as the packet is raw AAC data.
 *
 *  Note the packetLen must count in the ADTS header itself.
 **/

void addADTStoPacket(uint8_t *packet, int packetLen, int rate, int channel_configuration) {
  int profile = 2;
  int freqIdx = 4;
  if (rate == 44100)
    freqIdx = 4;
  else if (rate == 48000)
    freqIdx = 3;
  else
    debug(1, "Unsupported AAC sample rate %d.", rate);

  // Channel Configuration
  // https://wiki.multimedia.cx/index.php/MPEG-4_Audio#Channel_Configurations
  // clang-format off
  // 0: Defined in AOT Specifc Config
  // 1: 1 channel: front-center
  // 2: 2 channels: front-left, front-right
  // 3: 3 channels: front-center, front-left, front-right
  // 4: 4 channels: front-center, front-left, front-right, back-center
  // 5: 5 channels: front-center, front-left, front-right, back-left, back-right
  // 6: 6 channels: front-center, front-left, front-right, back-left, back-right, LFE-channel
  // 7: 8 channels: front-center, front-left, front-right, side-left, side-right, back-left, back-right, LFE-channel
  // 8-15: Reserved
  // clang-format on

  int chanCfg = channel_configuration; // CPE

  // fill in ADTS data
  packet[0] = 0xFF;
  packet[1] = 0xF9;
  packet[2] = ((profile - 1) << 6) + (freqIdx << 2) + (chanCfg >> 2);
  packet[3] = ((chanCfg & 3) << 6) + (packetLen >> 11);
  packet[4] = (packetLen & 0x7FF) >> 3;
  packet[5] = ((packetLen & 7) << 5) + 0x1F;
  packet[6] = 0xFC;
}

void rtp_buffered_audio_cleanup_handler(__attribute__((unused)) void *arg) {
  debug(2, "Buffered Audio Receiver Cleanup Start.");
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;
  close(conn->buffered_audio_socket);
  debug(2, "Connection %d: TCP Buffered Audio port closed: %u.", conn->connection_number,
        conn->local_buffered_audio_port);
  conn->buffered_audio_socket = 0;
  debug(2, "Connection %d: Buffered Audio Receiver Cleanup Done.", conn->connection_number);
}

void *rtp_buffered_audio_processor(void *arg) {
  //  #include <syscall.h>
  //  debug(1, "rtp_buffered_audio_processor PID %d", syscall(SYS_gettid));
  rtsp_conn_info *conn = (rtsp_conn_info *)arg;

  conn->incoming_ssrc = 0; // reset
  conn->resampler_ssrc = 0;
  pthread_cleanup_push(rtp_buffered_audio_cleanup_handler, arg);

  pthread_t *buffered_reader_thread = malloc(sizeof(pthread_t));
  if (buffered_reader_thread == NULL)
    debug(1, "cannot allocate a buffered_reader_thread!");
  memset(buffered_reader_thread, 0, sizeof(pthread_t));
  pthread_cleanup_push(malloc_cleanup, &buffered_reader_thread);

  buffered_tcp_desc *buffered_audio = malloc(sizeof(buffered_tcp_desc));
  if (buffered_audio == NULL)
    debug(1, "cannot allocate a buffered_tcp_desc!");
  // initialise the descriptor
  memset(buffered_audio, 0, sizeof(buffered_tcp_desc));
  pthread_cleanup_push(malloc_cleanup, &buffered_audio);

  if (pthread_mutex_init(&buffered_audio->mutex, NULL))
    debug(1, "Connection %d: error %d initialising buffered_audio mutex.", conn->connection_number,
          errno);
  pthread_cleanup_push(mutex_cleanup, &buffered_audio->mutex);

  if (pthread_cond_init(&buffered_audio->not_empty_cv, NULL))
    die("Connection %d: error %d initialising not_empty cv.", conn->connection_number, errno);
  pthread_cleanup_push(cv_cleanup, &buffered_audio->not_empty_cv);

  if (pthread_cond_init(&buffered_audio->not_full_cv, NULL))
    die("Connection %d: error %d initialising not_full cv.", conn->connection_number, errno);
  pthread_cleanup_push(cv_cleanup, &buffered_audio->not_full_cv);

  // initialise the buffer data structure
  buffered_audio->buffer_max_size = conn->ap2_audio_buffer_size;
  buffered_audio->buffer = malloc(conn->ap2_audio_buffer_size);
  if (buffered_audio->buffer == NULL)
    debug(1, "cannot allocate an audio buffer of %u bytes!", buffered_audio->buffer_max_size);
  pthread_cleanup_push(malloc_cleanup, &buffered_audio->buffer);

  // pthread_mutex_lock(&conn->buffered_audio_mutex);
  buffered_audio->toq = buffered_audio->buffer;
  buffered_audio->eoq = buffered_audio->buffer;

  buffered_audio->sock_fd = conn->buffered_audio_socket;

  named_pthread_create(buffered_reader_thread, NULL, &buffered_tcp_reader, buffered_audio,
                       "ap2_buf_rdr_%d", conn->connection_number);
  pthread_cleanup_push(thread_cleanup, buffered_reader_thread);

  const size_t leading_free_space_length =
      256; // leave this many bytes free to make room for prefixes that might be added later
  uint8_t packet[32 * 1024];
  unsigned char m[32 * 1024 + leading_free_space_length];

  unsigned char *payload_pointer = NULL;
  unsigned long long payload_length = 0;
  uint32_t payload_ssrc =
      SSRC_NONE; // this is the SSRC of the payload, needed to decide if it should be muted

  uint32_t seq_no =
      0; // audio packet number. Initialised to avoid a "possibly uninitialised" warning.
  int seqno_valid = 0;
  uint32_t previous_seqno = 0;
  int previous_seqno_valid = 0;

  int new_buffer_needed = 0;
  ssize_t nread;

  int finished = 0;

  uint64_t blocks_read = 0;
  // not used...
  // uint64_t blocks_read_in_sequence =
  //     0; // since the start of this sequence -- reset by start or flush
  int flush_requested = 0;

  uint32_t timestamp = 0; // initialised to avoid a "possibly uninitialised" warning.
  int timestamp_valid = 0;
  uint32_t previous_timestamp = 0;
  int previous_timestamp_valid = 0;

  uint32_t expected_timestamp = 0;
  int packets_played_in_this_sequence = 0;

  // uint32_t first_block_in_this_sequence = 0;
  uint32_t first_timestamp_in_this_sequence = 0;

  int play_enabled = 0;
  // uint32_t flush_from_timestamp = 0; // initialised to avoid a "possibly uninitialised" warning.
  double requested_lead_time = 0.0; // normal lead time minimum -- maybe  it should be about 0.1

  uint32_t old_ssrc = 0; // diagnostic

  // wait until our timing information is valid

  // debug(1,"rtp_buffered_audio_processor ready.");

  while (have_ptp_timing_information(conn) == 0)
    usleep(1000);

  reset_buffer(conn); // in case there is any garbage in the player

  do {
    uint16_t last_seqno_put = 0;
    int flush_is_delayed = 0;
    int flush_newly_requested = 0;
    int flush_newly_complete = 0;
    int play_newly_stopped = 0;
    // are we in in flush mode, or just about to leave it?

    pthread_cleanup_debug_mutex_lock(&conn->flush_mutex, 25000, 1); // 25 ms is a long time to wait!

    if ((play_enabled != 0) && (conn->ap2_play_enabled == 0)) {
      play_newly_stopped = 1;
      debug(2, "Play stopped.");
      // blocks_read_in_sequence =
      //     0; // This may be set to 1 by a flush, so don't zero it during start.
      packets_played_in_this_sequence = 0;
      new_buffer_needed = 0;
    }

    if ((play_enabled == 0) && (conn->ap2_play_enabled != 0)) {
      // play newly started
      debug(2, "Play started.");
    }

    play_enabled = conn->ap2_play_enabled;

    uint32_t flushUntilSeq = (conn->ap2_flush_until_sequence_number - 1) & 0x7fffff;
    uint32_t flushUntilTS = conn->ap2_flush_until_rtp_timestamp;

    int flush_request_active = 0;
    if (conn->ap2_flush_requested) {
      if (conn->ap2_flush_from_valid == 0) { // i.e. a flush from right now
        if (play_enabled)
          debug(2, "Connection %d: immediate flush activated while play_enabled is true.",
                conn->connection_number);
        flush_request_active = 1;
        flush_is_delayed = 0;
      } else {
        flush_is_delayed = 1;
        // flush_from_timestamp = conn->ap2_flush_from_rtp_timestamp;
        int32_t blocks_to_start_of_flush = conn->ap2_flush_from_sequence_number - seq_no;
        if (blocks_to_start_of_flush <= 0) {
          debug(3, "Connection %d: deferred flush activated.", conn->connection_number);
          if (play_enabled)
            debug(3, "Connection %d: deferred flush activated while play_enabled is true.",
                  conn->connection_number);
          flush_request_active = 1;
        }
      }
    }
    // if we are in flush mode
    if (flush_request_active) {
      if (flush_requested == 0) {
        // here, a flush has been newly requested

        debug(3, "Connection %d: Flush requested.", conn->connection_number);
        if (conn->ap2_flush_from_valid) {
          debug(3, "  fromTS:          %u", conn->ap2_flush_from_rtp_timestamp);
          debug(3, "  fromSeq:         %u", conn->ap2_flush_from_sequence_number);
          debug(3, "--");
        }
        debug(3, "  untilTS:         %u", conn->ap2_flush_until_rtp_timestamp);
        debug(3, "  untilSeq:        %u", conn->ap2_flush_until_sequence_number);
        debug(3, "--");
        debug(3, "  currentTS_Start: %u", timestamp);
        debug(3, "  currentSeq:      %u", seq_no);

        flush_newly_requested = 1;
      }
      // blocks_read to ensure seq_no is valid
      // (seq_no - flushUntilSeq) & 0x400000 -- if this is 0, seq_no is >= flushUntilSeq
      if ((blocks_read != 0) && (((seq_no - flushUntilSeq) & 0x400000) == 0)) {
        // we have reached or overshot the flushUntilSeq block
        if (seq_no == flushUntilSeq) { // if the flush ended as expected, just before
                                       // conn->ap2_flush_until_sequence_number
          debug(3,
                "Connection %d: flush request ended normally at %u/%u with "
                "ap2_flush_until_sequence_number: %u/%u, "
                "flushUntilTS: %u, incoming "
                "timestamp: %u",
                conn->connection_number, seq_no, seq_no & 0xffff,
                conn->ap2_flush_until_sequence_number,
                conn->ap2_flush_until_sequence_number & 0xffff, flushUntilTS, timestamp);
        } else {
          // sometimes, the block number jumps directly to the
          // conn->ap2_flush_until_sequence_number, skipping the preceding one (which is what is in
          // flushUntilSeq...)
          if (seq_no == conn->ap2_flush_until_sequence_number)
            debug(3,
                  "Connection %d: flush request ended normally at %u/%u with "
                  "ap2_flush_until_sequence_number: %u/%u, "
                  "flushUntilTS: %u, incoming "
                  "timestamp: %u",
                  conn->connection_number, seq_no, seq_no & 0xffff,
                  conn->ap2_flush_until_sequence_number,
                  conn->ap2_flush_until_sequence_number & 0xffff, flushUntilTS, timestamp);
          else
            debug(1,
                  "Connection %d: flush request ended with a discontinuity at %u/%u with "
                  "ap2_flush_until_sequence_number: %u/%u, "
                  "flushUntilTS: %u, incoming "
                  "timestamp: %u",
                  conn->connection_number, seq_no, seq_no & 0xffff,
                  conn->ap2_flush_until_sequence_number,
                  conn->ap2_flush_until_sequence_number & 0xffff, flushUntilTS, timestamp);
          new_buffer_needed = 0; // use this first block in the new sequence
        }
        conn->ap2_flush_requested = 0;
        flush_request_active = 0;
        flush_newly_requested = 0;
      }
    }

    if ((flush_requested) && (flush_request_active == 0)) {
      if (play_enabled)
        debug(3, "Connection %d: flush completed while play_enabled is true.",
              conn->connection_number);
      flush_newly_complete = 1;
      // blocks_read_in_sequence =
      //     1; // the last block always (?) becomes the first block after the flush
    }
    flush_requested = flush_request_active; // for next time...

    // debug_mutex_unlock(&conn->flush_mutex, 3);
    pthread_cleanup_pop(1); // the mutex

    // do this outside the flush mutex
    if (flush_newly_complete) {
      debug(3, "Connection %d: flush complete.", conn->connection_number);
    }

    if (play_newly_stopped != 0)
      reset_buffer(conn); // stop play ASAP

    if (flush_newly_requested) {
      reset_buffer(
          conn); // stop play when an immediate flush starts or when a deferred flush is activated.
      if (flush_is_delayed == 0) {
        debug(3, "Connection %d: immediate buffered audio flush started.", conn->connection_number);
        packets_played_in_this_sequence = 0;
      } else {
        debug(1, "Connection %d: deferred buffered audio flush started.", conn->connection_number);
        packets_played_in_this_sequence = 0;
      }
    }

    // now, if a flush is not requested, see if we need to get a block
    if (flush_requested == 0) {

      // is there space in the player thread's buffer system?
      unsigned int player_buffer_size, player_buffer_occupancy;
      get_audio_buffer_size_and_occupancy(&player_buffer_size, &player_buffer_occupancy, conn);
      // debug(1,"player buffer size and occupancy: %u and %u", player_buffer_size,
      // player_buffer_occupancy);

      if ((play_enabled != 0) &&
          (player_buffer_occupancy <= 2 * ((config.audio_backend_buffer_desired_length) *
                                           conn->input_rate / conn->frames_per_packet)) &&
          (payload_pointer == NULL) &&
          (flush_requested == 0)) { // must be greater than the lead time
        new_buffer_needed = 1;
      } else {
        usleep(20000); // wait for a while
      }
    }

    int64_t lead_time = 0;

    // so we need to read a block, as either a flush or a new buffer is needed...
    if ((flush_requested) || (new_buffer_needed)) {

      // start here to read (and later, decipher) a block.

      // a block is preceded by its length in a uint16_t
      uint16_t data_len;
      // here we read from the buffer that our thread has been reading

      size_t bytes_remaining_in_buffer;
      nread = lread_sized_block(buffered_audio, &data_len, sizeof(data_len),
                                &bytes_remaining_in_buffer);
      data_len = ntohs(data_len);
      // diagnostic
      if ((conn->ap2_audio_buffer_minimum_size < 0) ||
          (bytes_remaining_in_buffer < (size_t)conn->ap2_audio_buffer_minimum_size))
        conn->ap2_audio_buffer_minimum_size = bytes_remaining_in_buffer;

      // if (flush_requested)
      //   debug(1, "read %u bytes for a flush of a block length of %u.", nread, data_len);

      if (nread > 0) {
        // get the block itself
        // debug(1,"buffered audio packet of size %u detected.", data_len - 2);
        nread = lread_sized_block(buffered_audio, packet, data_len - 2, &bytes_remaining_in_buffer);

        // diagnostic
        if ((conn->ap2_audio_buffer_minimum_size < 0) ||
            (bytes_remaining_in_buffer < (size_t)conn->ap2_audio_buffer_minimum_size))
          conn->ap2_audio_buffer_minimum_size = bytes_remaining_in_buffer;
        // debug(1, "buffered audio packet of size %u received.", nread);
        if (nread > 0) {

          // got the block
          blocks_read++; // note, this doesn't mean they are valid audio blocks
          // blocks_read_in_sequence++;

          // get the sequence number
          // see https://en.wikipedia.org/wiki/Real-time_Transport_Protocol#Packet_header
          // the Marker bit is always set, and it and the remaining 23 bits form the sequence number

          if (seqno_valid) {
            previous_seqno = seq_no;
            previous_seqno_valid = 1;
          }

          seq_no = nctohl(&packet[0]) & 0x7FFFFF;
          seqno_valid = 1;

          // if (flush_requested)
          //   debug(1, "read %u bytes for a flush of block %u up to block %u.", nread, seq_no,
          //   conn->ap2_flush_until_sequence_number);

          // int unexpected_seqno = 0;

          if (previous_seqno_valid) {
            uint32_t t_expected_seqno = (previous_seqno + 1) & 0x7fffff;
            if (t_expected_seqno != seq_no) {
              // unexpected_seqno = 1;
              if (flush_requested == 0)
                debug(1, "seq_no %u differs from expected_seq_no %u.", seq_no, t_expected_seqno);
            }
          }

          // timestamp
          if (timestamp_valid) {
            previous_timestamp_valid = 1;
            previous_timestamp = timestamp;
          }

          timestamp = nctohl(&packet[4]);

          if (previous_timestamp_valid) {
            uint32_t t_expected_timestamp = previous_timestamp + conn->frames_per_packet;
            if (t_expected_timestamp != timestamp)
              debug(1, "timestamp %u differs from expected_timestamp %u.", timestamp,
                    t_expected_timestamp);
          }

          // debug(1,"seqno: %u, seqno16: %u, timestamp: %u.", seq_no, seq_no & 0xffff, timestamp);

          payload_ssrc = nctohl(&packet[8]);

          if ((payload_ssrc != old_ssrc) && (payload_ssrc != SSRC_NONE) &&
              (old_ssrc != SSRC_NONE)) {
            if (ssrc_is_recognised(payload_ssrc) == 0)
              debug(1, "Unrecognised SSRC: %u.", payload_ssrc);
            else
              debug(3, "Reading a block: new encoding: %s, old encoding: %s.",
                    get_ssrc_name(payload_ssrc), get_ssrc_name(old_ssrc));
          }
          old_ssrc = payload_ssrc;

          prepare_decoding_chain(conn, payload_ssrc);

          // change the (0) to (1) to process blocks with unrecognised SSRCs
          if ((1) && (ssrc_is_recognised(payload_ssrc) == 0)) {
            unsigned char nonce[12];
            memset(nonce, 0, sizeof(nonce));
            memcpy(nonce + 4, packet + nread - 8,
                   8); // front-pad the 8-byte nonce received to get the 12-byte nonce expected
            int response = crypto_aead_chacha20poly1305_ietf_decrypt(
                m,               // m
                &payload_length, // mlen_p
                NULL,            // nsec,
                packet + 12, // the ciphertext starts 12 bytes in and is followed by the MAC tag,
                nread - (8 + 12), // clen -- the last 8 bytes are the nonce
                packet + 4,       // authenticated additional data
                8,                // authenticated additional data length
                nonce,
                conn->session_key); // *k
            if (response != 0) {
              debug(
                  1,
                  "Can't decipher block %u with ssrc \"%s\". Byte length: %u bytes,  timestamp: %u",
                  seq_no, get_ssrc_name(payload_ssrc), payload_length, data_len);
            } else {
              if (payload_length == 0) {
                debug(2, "packet %u: unrecognised SSRC %u, and, when deciphered, has no content.",
                      seq_no & 0xFFFF, payload_ssrc);
              } else {
                debug(1,
                      "packet %u: unrecognised SSRC %u, packet length %d, deciphered length %lld. "
                      "Raw contents and then deciphered contents follow:",
                      seq_no & 0xFFFF, payload_ssrc, nread, payload_length);
                debug_print_buffer(1, packet, nread);
                debug_print_buffer(1, m, payload_length);
              }
            }
          }

          uint64_t buffer_should_be_time;
          frame_to_local_time(timestamp, &buffer_should_be_time, conn);
          lead_time = buffer_should_be_time - get_absolute_time_in_ns();
          payload_pointer = NULL;
          payload_length = 0;

          // decipher it only if it's needed, i.e. if it is not to be discarded

          // if a new buffer is needed, the block needs to be deciphered

          // if ((new_buffer_needed) || ((flush_requested != 0) && (unexpected_seqno != 0))) {
          if (new_buffer_needed) {
            // debug(1,"nbn seqno: %u, seqno16: %u, timestamp: %u.", seq_no, seq_no & 0xffff,
            // timestamp);
            if (ssrc_is_recognised(payload_ssrc) != 0) {
              prepare_decoding_chain(conn, payload_ssrc);
              unsigned long long new_payload_length = 0;
              payload_pointer = m + leading_free_space_length;
              if ((lead_time < (int64_t)30000000000L) &&
                  (lead_time >= 0)) { // only decipher the packet if it's not too late or too early
                int response = -1;    // guess that there is a problem
                if (conn->session_key != NULL) {
                  unsigned char nonce[12];
                  memset(nonce, 0, sizeof(nonce));
                  memcpy(
                      nonce + 4, packet + nread - 8,
                      8); // front-pad the 8-byte nonce received to get the 12-byte nonce expected

                  // https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/chacha20-poly1305/ietf_chacha20-poly1305_construction
                  // Note: the eight-byte nonce must be front-padded out to 12 bytes.

                  // Leave leading_free_space_length bytes at the start for possible headers like an
                  // ADTS header (7 bytes)
                  memset(m, 0, leading_free_space_length);
                  response = crypto_aead_chacha20poly1305_ietf_decrypt(
                      payload_pointer,     // where the decrypted payload will start
                      &new_payload_length, // mlen_p
                      NULL,                // nsec,
                      packet +
                          12, // the ciphertext starts 12 bytes in and is followed by the MAC tag,
                      nread - (8 + 12), // clen -- the last 8 bytes are the nonce
                      packet + 4,       // authenticated additional data
                      8,                // authenticated additional data length
                      nonce,
                      conn->session_key); // *k
                  if (response != 0)
                    debug(1, "Error decrypting audio packet %u -- packet length %d.", seq_no,
                          nread);
                } else {
                  debug(2, "No session key, so the audio packet can not be deciphered -- skipped.");
                }

                if ((response == 0) && (new_payload_length > 0)) {
                  // now we have the deciphered block, so send it to the player if we can
                  payload_length = new_payload_length;

                  if (ssrc_is_aac(payload_ssrc)) {
                    payload_pointer =
                        payload_pointer - 7; // including the 7-byte leader for the ADTS
                    payload_length = payload_length + 7;

                    // now, fill in the 7-byte ADTS information, which seems to be needed by the
                    // decoder we made room for it in the front of the buffer by filling from m + 7.
                    int channelConfiguration = 2; // 2: 2 channels: front-left, front-right
                    if (payload_ssrc == AAC_48000_F24_5P1)
                      channelConfiguration = 6; // 6: 6 channels: front-center, front-left,
                                                // front-right, back-left, back-right, LFE-channel
                    else if (payload_ssrc == AAC_48000_F24_7P1)
                      channelConfiguration =
                          7; // 7: 8 channels: front-center, front-left, front-right,
                             // side-left, side-right, back-left, back-right, LFE-channel
                    addADTStoPacket(payload_pointer, payload_length, conn->input_rate,
                                    channelConfiguration);
                  }
                  // debug(1, "creating seqno %u, seqno16 %u, with timestamp %u, leadtime %f.",
                  // seq_no,
                  //  seq_no & 0xffff, timestamp, lead_time * 0.000000001);
                }
              }
            } else {
              debug(3, "Unrecognised or invalid ssrc: %s, packet length %d.",
                    get_ssrc_name(payload_ssrc), nread);
            }
          } else {
            if (seq_no % 10 == 0)
              debug(3, "Dropping seqno %u, seqno16 %u, with timestamp %u, leadtime %f.", seq_no,
                    seq_no & 0xffff, timestamp, lead_time * 0.000000001);
          }
        }
      }

      if (nread == 0) {
        // nread is 0 -- the port has been closed
        debug(3, "buffered audio port closed!");
        finished = 1;
      } else if (nread < 0) {
        char errorstring[1024];
        strerror_r(errno, (char *)errorstring, sizeof(errorstring));
        debug(1, "error in rtp_buffered_audio_processor %d: \"%s\". Could not recv a data_len .",
              errno, errorstring);
        finished = 1;
      }
    }

    if ((play_enabled != 0) && (payload_pointer != NULL) && (finished == 0) &&
        (new_buffer_needed != 0)) {

      // it seems that some garbage blocks can be left after the flush, so
      // only accept them if they have sensible lead times
      if ((lead_time < (int64_t)30000000000L) && (lead_time >= 0)) {
        // if it's the very first block (thus no priming needed)
        // if ((blocks_read == 1) || (blocks_read_in_sequence > 3)) {
        if ((lead_time >= (int64_t)(requested_lead_time * 1000000000L)) ||
            (packets_played_in_this_sequence != 0)) {
          int mute = ((packets_played_in_this_sequence == 0) && (ssrc_is_aac(payload_ssrc)));
          // if (mute) {
          //   debug(1, "Muting first AAC block, timestamp %u.", timestamp);
          // }
          int32_t timestamp_difference = 0;
          if (packets_played_in_this_sequence == 0) {
            // first_block_in_this_sequence = seq_no;
            first_timestamp_in_this_sequence = timestamp;
          } else {
            timestamp_difference = timestamp - expected_timestamp;
            if (timestamp_difference != 0) {
              debug(
                  1,
                  "Connection %d: "
                  "unexpected timestamp in packet %u. Actual: %u, expected: %u difference: %d, "
                  "%f ms. "
                  "Positive means later, i.e. a gap. First timestamp was %u, payload type: \"%s\".",
                  conn->connection_number, seq_no & 0xffff, timestamp, expected_timestamp,
                  timestamp_difference, 1000.0 * timestamp_difference / conn->input_rate,
                  first_timestamp_in_this_sequence, get_ssrc_name(payload_ssrc));
              // mute the first packet after a discontinuity
              if (ssrc_is_aac(payload_ssrc)) {
                // debug(1, "Muting first AAC block after a timestamp discontinuity, timestamp %u.",
                // timestamp);
                mute = 1;
              }
            }
          }

          // frames_per_packet should be set during setup.
          // if ((timestamp_difference >= 0) || (conn->frames_per_packet + timestamp_difference >
          // 0)) {
          if (timestamp_difference < 0)
            debug(3,
                  "The next %d frames are late, even though the sequence numbers are in line. The "
                  "\"late\" frames should be decoded to prime the AAC decoder and then dropped.",
                  -timestamp_difference);
          uint32_t packet_size =
              player_put_packet(payload_ssrc, seq_no & 0xFFFF, timestamp, payload_pointer,
                                payload_length, mute, timestamp_difference, conn);
          // debug(1, "put packet: %u, packets_played_in_this_sequence is %d.", seq_no & 0xFFFF,
          // packets_played_in_this_sequence);
          if (last_seqno_put != 0) {
            uint16_t seqno_expected = last_seqno_put + 1;
            if (seqno_expected != (seq_no & 0xffff))
              debug(1, "Packet puts not in sequence. Expected: %u, actual: %u.", seqno_expected,
                    (seq_no & 0xffff));
          }
          last_seqno_put = seq_no & 0xffff;
          expected_timestamp = timestamp + packet_size; // for the next time
          new_buffer_needed = 0;
          packets_played_in_this_sequence++;
          /*
          } else {
             // expected_seq_no = seq_no + 1;
             debug(1,
                   "Connection %d: "
                   "dropping packet %u, seq_no %u, that arrived %.3f ms later than "
                   "expected due to a discontinuity in the timestamp sequence. First "
                   "timestamp was %u.",
                   conn->connection_number, seq_no, timestamp,
                   -1000.0 * timestamp_difference / conn->input_rate,
                   first_timestamp_in_this_sequence);
           }
           */
        }
        // }
      } else {
        debug(3,
              "Dropping packet %u from seqno %u, seqno16 %u, with out-of-range lead_time: %.3f "
              "seconds.",
              timestamp, seq_no, seq_no & 0xffff, 0.000000001 * lead_time);
        expected_timestamp = timestamp + conn->frames_per_packet; // for the next time
      }
      payload_pointer = NULL; // payload consumed
    }
  } while (finished == 0);
  debug(2, "Buffered Audio Receiver RTP thread \"normal\" exit.");
  pthread_cleanup_pop(1); // thread creation
  pthread_cleanup_pop(1); // buffer malloc
  pthread_cleanup_pop(1); // not_full_cv
  pthread_cleanup_pop(1); // not_empty_cv
  pthread_cleanup_pop(1); // mutex
  pthread_cleanup_pop(1); // descriptor malloc
  pthread_cleanup_pop(1); // pthread_t malloc
  pthread_cleanup_pop(1); // do the cleanup.
  pthread_exit(NULL);
}

int frame_to_local_time(uint32_t timestamp, uint64_t *time, rtsp_conn_info *conn) {
  if (conn->timing_type == ts_ptp)
    return frame_to_ptp_local_time(timestamp, time, conn);
  else
    return frame_to_ntp_local_time(timestamp, time, conn);
}

int local_time_to_frame(uint64_t time, uint32_t *frame, rtsp_conn_info *conn) {
  if (conn->timing_type == ts_ptp)
    return local_ptp_time_to_frame(time, frame, conn);
  else
    return local_ntp_time_to_frame(time, frame, conn);
}

void reset_anchor_info(rtsp_conn_info *conn) {
  if (conn->timing_type == ts_ptp)
    reset_ptp_anchor_info(conn);
  else
    reset_ntp_anchor_info(conn);
}

int have_timestamp_timing_information(rtsp_conn_info *conn) {
  if (conn->timing_type == ts_ptp)
    return have_ptp_timing_information(conn);
  else
    return have_ntp_timing_information(conn);
}

#else

int frame_to_local_time(uint32_t timestamp, uint64_t *time, rtsp_conn_info *conn) {
  return frame_to_ntp_local_time(timestamp, time, conn);
}

int local_time_to_frame(uint64_t time, uint32_t *frame, rtsp_conn_info *conn) {
  return local_ntp_time_to_frame(time, frame, conn);
}

void reset_anchor_info(rtsp_conn_info *conn) { reset_ntp_anchor_info(conn); }

int have_timestamp_timing_information(rtsp_conn_info *conn) {
  return have_ntp_timing_information(conn);
}
#endif
