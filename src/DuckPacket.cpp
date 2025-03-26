#include "Arduino.h"
#include "include/DuckPacket.h"
#include "DuckError.h"
#include "DuckLogger.h"
#include "MemoryFree.h"
#include "include/DuckUtils.h"
#include "include/DuckCrypto.h"
#include "include/bloomfilter.h"
#include <string>


bool DuckPacket::prepareForRelaying(BloomFilter *filter, std::vector<byte> dataBuffer) {


  this->reset();

  loginfo_ln("prepareForRelaying: START");
  loginfo_ln("prepareForRelaying: Packet is built. Checking for relay...");

  // Query the existence of strings
  bool alreadySeen = filter->bloom_check(&dataBuffer[MUID_POS], MUID_LENGTH);
  if (alreadySeen) {
    logdbg_ln("handleReceivedPacket: Packet already seen. No relay.");
    return false;
  } else {
    filter->bloom_add(&dataBuffer[MUID_POS], MUID_LENGTH);
    logdbg_ln("handleReceivedPacket: Relaying packet: %s", duckutils::convertToHex(&dataBuffer[MUID_POS], MUID_LENGTH).c_str());
  }

  // update the rx packet internal byte buffer
  buffer = dataBuffer;
  int hops = buffer[HOP_COUNT_POS]++;
  loginfo_ln("prepareForRelaying: hops count: %d", hops);
  return true;
  
  
}

void DuckPacket::getUniqueMessageId(BloomFilter * filter, byte message_id[MUID_LENGTH]) {

  bool getNewUnique = true;
  while (getNewUnique) {
    duckutils::getRandomBytes(MUID_LENGTH, message_id);
    getNewUnique = filter->bloom_check(message_id, MUID_LENGTH);
    loginfo_ln("prepareForSending: new MUID -> %s",duckutils::convertToHex(message_id, MUID_LENGTH).c_str());
  }
}

int DuckPacket::prepareForSending(BloomFilter *filter,
                                  std::array<byte,8> targetDevice, byte duckType,
                                  byte topic, std::vector<byte> app_data) {

  std::vector<uint8_t> encryptedData;
  uint8_t app_data_length = app_data.size();

  this->reset();

  if ( app_data.empty() || app_data_length > MAX_DATA_LENGTH) {
    return DUCKPACKET_ERR_SIZE_INVALID;
  }

  loginfo_ln("prepareForSending: DATA LENGTH: %d - TOPIC (%s)", app_data_length, CdpPacket::topicToString(topic).c_str());

  byte message_id[MUID_LENGTH];
  getUniqueMessageId(filter, message_id);

  byte crc_bytes[DATA_CRC_LENGTH];
  uint32_t value;
  // TODO: update the CRC32 library to return crc as a byte array
  if(duckcrypto::getState()) {
    duckcrypto::encryptData(app_data.data(), encryptedData.data(), app_data.size());
    value = CRC32::calculate(encryptedData.data(), encryptedData.size());
  } else {
    value = CRC32::calculate(app_data.data(), app_data.size());
  }
  
  crc_bytes[0] = (value >> 24) & 0xFF;
  crc_bytes[1] = (value >> 16) & 0xFF;
  crc_bytes[2] = (value >> 8) & 0xFF;
  crc_bytes[3] = value & 0xFF;

  // ----- insert packet header  -----
  // source device uid
    buffer.insert(buffer.end(), duid.begin(), duid.end());
    logdbg_ln("SDuid:     %s",duckutils::convertToHex(duid.data(), duid.size()).c_str());

    // destination device uid
    buffer.insert(buffer.end(), targetDevice.begin(), targetDevice.end());
    logdbg_ln("DDuid:     %s", duckutils::convertToHex(targetDevice.data(), targetDevice.size()).c_str());

    // message uid
    buffer.insert(buffer.end(), &message_id[0], &message_id[MUID_LENGTH]);
    logdbg_ln("Muid:      %s", duckutils::convertToHex(buffer.data(), buffer.size()).c_str());

    // topic
    buffer.insert(buffer.end(), topic);
    logdbg_ln("Topic:     %s", duckutils::convertToHex(buffer.data(), buffer.size()).c_str());

    // duckType
    buffer.insert(buffer.end(), duckType);
    logdbg_ln("duck type: %s", duckutils::convertToHex(buffer.data(), buffer.size()).c_str());

    // hop count
    buffer.insert(buffer.end(), 0x00);
    logdbg_ln("hop count: %s", duckutils::convertToHex(buffer.data(), buffer.size()).c_str());

    // data crc
    buffer.insert(buffer.end(), &crc_bytes[0], &crc_bytes[DATA_CRC_LENGTH]);
    logdbg_ln("Data CRC:  %s", duckutils::convertToHex(buffer.data(), buffer.size()).c_str());

    // ----- insert data -----
    if(duckcrypto::getState()) {

        buffer.insert(buffer.end(), encryptedData.begin(), encryptedData.end());
        logdbg_ln("Encrypted Data:      %s", duckutils::convertToHex(buffer.data(), buffer.size()).c_str());

    } else {
        buffer.insert(buffer.end(), app_data.begin(), app_data.end());
        logdbg_ln("Data:      %s",duckutils::convertToHex(buffer.data(), buffer.size()).c_str());
    }

    logdbg_ln("Built packet: %s", duckutils::convertToHex(buffer.data(), buffer.size()).c_str());
    getBuffer().shrink_to_fit();
  return DUCK_ERR_NONE;
}

int DuckPacket::addToBuffer(std::vector<byte> additional_data) {
  
  loginfo_ln("Adding RSSI and SNR data");
  
  // Ensure total data doesn't exceed max length
  if (buffer.size() + additional_data.size() > PACKET_LENGTH) {
      return DUCKPACKET_ERR_SIZE_INVALID;
  }

  loginfo_ln("Buffer doesn't exceed max limit!");
  
  buffer.insert(buffer.end(), additional_data.begin(), additional_data.end());
  logdbg_ln("NewData:      %s",duckutils::convertToHex(buffer.data(), buffer.size()).c_str());

  std::vector<byte> dataPortion(buffer.begin() + HEADER_LENGTH, buffer.end());
  
  // Recalculate CRC
  uint32_t value = CRC32::calculate(dataPortion.data(), dataPortion.size());
  
  // Update CRC bytes in-place
  buffer[DATA_CRC_POS] = (value >> 24) & 0xFF;
  buffer[DATA_CRC_POS + 1] = (value >> 16) & 0xFF;
  buffer[DATA_CRC_POS + 2] = (value >> 8) & 0xFF;
  buffer[DATA_CRC_POS + 3] = value & 0xFF;

  return DUCK_ERR_NONE;
}

int DuckPacket::addMetrics(int RSSI, float SNR) {
  // Store original data length before adding metrics
  size_t originalDataLength = buffer.size() - HEADER_LENGTH;
  
  int snrInt = static_cast<int>(SNR * 10);
  logdbg_ln("addMetrics: RSSI = %d, SNR = %d", RSSI, snrInt);
  // Create metrics data
  std::vector<byte> additional_data;
  additional_data.push_back((RSSI >> 8) & 0xFF);  
  additional_data.push_back(RSSI & 0xFF);         
  additional_data.push_back((snrInt >> 8) & 0xFF);   
  additional_data.push_back(snrInt & 0xFF);          

  // Check total size
  if (buffer.size() + additional_data.size() > PACKET_LENGTH) {
      return DUCKPACKET_ERR_SIZE_INVALID;
  }

  // Extract current data (excluding header)
  std::vector<byte> currentData(buffer.begin() + HEADER_LENGTH, buffer.end());
  
  // Append new metrics
  currentData.insert(currentData.end(), additional_data.begin(), additional_data.end());

  // Calculate new CRC for all data including metrics
  uint32_t value = CRC32::calculate(currentData.data(), currentData.size());
  
  // Update CRC in header
  buffer[DATA_CRC_POS] = (value >> 24) & 0xFF;
  buffer[DATA_CRC_POS + 1] = (value >> 16) & 0xFF;
  buffer[DATA_CRC_POS + 2] = (value >> 8) & 0xFF;
  buffer[DATA_CRC_POS + 3] = value & 0xFF;

  // Add metrics after updating CRC
  buffer.insert(buffer.end(), additional_data.begin(), additional_data.end());

  logdbg_ln("Successful in adding RSSI and SNR");

  return DUCK_ERR_NONE;
}