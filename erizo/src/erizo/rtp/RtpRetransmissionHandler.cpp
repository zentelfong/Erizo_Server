#include "rtp/RtpRetransmissionHandler.h"

#include <algorithm>

#include "rtp/RtpUtils.h"

namespace erizo {

DEFINE_LOGGER(RtpRetransmissionHandler, "rtp.RtpRetransmissionHandler");

constexpr float kDefaultBitrate = 300000.;

RtpRetransmissionHandler::RtpRetransmissionHandler(std::shared_ptr<erizo::Clock> the_clock)
  : clock_{the_clock},
    connection_{nullptr},
    audio_{kRetransmissionsBufferSize},
    video_{kRetransmissionsBufferSize},
    bucket_{static_cast<uint64_t>(kDefaultBitrate * kMarginRtxBitrate), kBurstSize, clock_},
    last_bitrate_time_{clock_->now()} {}


void RtpRetransmissionHandler::enable() {
}

void RtpRetransmissionHandler::disable() {
}

void RtpRetransmissionHandler::notifyUpdate() {
  auto pipeline = getContext()->getPipelineShared();
  if (pipeline && !connection_) {
    connection_ = pipeline->getService<WebRtcConnection>().get();
    stats_ = pipeline->getService<Stats>();
  }
}

MovingIntervalRateStat& RtpRetransmissionHandler::getRtxBitrateStat() {
  if (!stats_->getNode()["total"].hasChild("rtxBitrate")) {
    stats_->getNode()["total"].insertStat("rtxBitrate",
        MovingIntervalRateStat{std::chrono::milliseconds(100), 30, 8.});
  }
  return static_cast<erizo::MovingIntervalRateStat&>(stats_->getNode()["total"]["rtxBitrate"]);
}

uint64_t RtpRetransmissionHandler::getBitrateCalculated() {
  if (!stats_->getNode()["total"].hasChild("bitrateCalculated")) {
    return 0;
  }
  return stats_->getNode()["total"]["bitrateCalculated"].value() / 8.;
}

void RtpRetransmissionHandler::calculateRtxBitrate() {
  time_point now = clock_->now();
  if (now - last_bitrate_time_ > kTimeToUpdateBitrate) {
    last_bitrate_time_ = now;
    bucket_.reset(getBitrateCalculated() * kMarginRtxBitrate, kBurstSize);
  }
}

void RtpRetransmissionHandler::read(Context *ctx, std::shared_ptr<dataPacket> packet) {
  // ELOG_DEBUG("%p READING %d bytes", this, packet->length);
  calculateRtxBitrate();

  bool contains_nack = false;
  bool is_fully_recovered = true;

  RtpUtils::forEachRRBlock(packet, [this, &contains_nack, &is_fully_recovered](RtcpHeader *chead) {
    if (chead->packettype == RTCP_RTP_Feedback_PT) {
      contains_nack = true;

      RtpUtils::forEachNack(chead, [this, chead, &is_fully_recovered](uint16_t new_seq_num, uint16_t new_plb) {
          uint16_t initial_seq_num = new_seq_num;
          uint16_t plb = new_plb;

          for (int i = -1; i <= kNackBlpSize; i++) {
            uint16_t seq_num = initial_seq_num + i + 1;
            bool packet_nacked = i == -1 || (plb >> i) & 0x0001;

            if (packet_nacked) {
              std::shared_ptr<dataPacket> recovered;

              if (connection_->getVideoSinkSSRC() == chead->getSourceSSRC()) {
                recovered = video_[getIndexInBuffer(seq_num)];
              } else if (connection_->getAudioSinkSSRC() == chead->getSourceSSRC()) {
                recovered = audio_[getIndexInBuffer(seq_num)];
              }

              if (recovered.get()) {
                if (!bucket_.consume(recovered->length)) {
                  continue;
                }
                RtpHeader *recovered_head = reinterpret_cast<RtpHeader*> (recovered->data);
                if (recovered_head->getSeqNumber() == seq_num) {
                  getRtxBitrateStat() += recovered->length;
                  getContext()->fireWrite(recovered);
                  continue;
                }
              }
              ELOG_DEBUG("Packet missed in buffer %d", seq_num);
              is_fully_recovered = false;
            }
          }
        });
    }
  });
  if (!contains_nack || !is_fully_recovered) {
    ctx->fireRead(packet);
  }
}

void RtpRetransmissionHandler::write(Context *ctx, std::shared_ptr<dataPacket> packet) {
  // ELOG_DEBUG("%p WRITING %d bytes", this, packet->length);
  RtpHeader *head = reinterpret_cast<RtpHeader*> (packet->data);
  RtcpHeader *chead = reinterpret_cast<RtcpHeader*> (packet->data);
  if (!chead->isRtcp()) {
    if (connection_->getVideoSinkSSRC() == head->getSSRC()) {
      video_[getIndexInBuffer(head->getSeqNumber())] = packet;
    } else if (connection_->getAudioSinkSSRC() == head->getSSRC()) {
      audio_[getIndexInBuffer(head->getSeqNumber())] = packet;
    }
  }
  ctx->fireWrite(packet);
}

uint16_t RtpRetransmissionHandler::getIndexInBuffer(uint16_t seq_num) {
  return seq_num % kRetransmissionsBufferSize;
}
}  // namespace erizo
