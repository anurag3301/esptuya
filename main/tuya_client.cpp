#include "tuya_client.hpp"

#include <cerrno>
#include <ctime>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

constexpr int MAX_BUFFER_SIZE = 1024;
constexpr uint32_t MESSAGE_PREFIX = 0x00006699;
constexpr uint32_t MESSAGE_SUFFIX = 0x00009966;
constexpr int PROTOCOL_35_HEADER_SIZE = 18;
constexpr int MESSAGE_TRAILER_SIZE = 4;
constexpr int GCM_TAG_SIZE = 16;
constexpr int GCM_IV_SIZE = 12;
constexpr int TUYA_COMMAND_PORT = 6668;

// Tuya Command Types (subset used here)
constexpr uint8_t TUYA_DP_QUERY_NEW = 16;
constexpr uint8_t TUYA_HEART_BEAT = 9;
constexpr uint8_t TUYA_CONTROL_NEW = 13;

class TuyaProtocol35 {
public:
	TuyaProtocol35()
	{
		m_session_established = false;
		m_seqno = 0;
		memset(m_session_key, 0, sizeof(m_session_key));
		memset(m_local_nonce, 0, sizeof(m_local_nonce));
		memset(m_remote_nonce, 0, sizeof(m_remote_nonce));
	}

	void SetEncryptionKey(const std::string &key)
	{
		m_encryption_key = key;
		m_session_established = false;
		m_seqno = 0;
		random_bytes(m_local_nonce, 16);
	}

	bool isSessionEstablished() const { return m_session_established; }

	int BuildTuyaMessage(unsigned char *buffer, const uint8_t command, const std::string &szPayload)
	{
		if (!m_session_established)
			return -1;

		m_seqno++;

		std::string payload = szPayload;
		if (command == TUYA_CONTROL_NEW) {
			payload = "3.5";
			payload.append(12, '\0');
			payload.append(szPayload);
		}
		// DP query uses payload as-is

		unsigned char iv[GCM_IV_SIZE];
		random_bytes(iv, GCM_IV_SIZE);

		int bufferpos = 0;
		memset(buffer, 0, PROTOCOL_35_HEADER_SIZE);
		buffer[0] = (MESSAGE_PREFIX & 0xFF000000) >> 24;
		buffer[1] = (MESSAGE_PREFIX & 0x00FF0000) >> 16;
		buffer[2] = (MESSAGE_PREFIX & 0x0000FF00) >> 8;
		buffer[3] = (MESSAGE_PREFIX & 0x000000FF);
		buffer[6] = (m_seqno & 0xFF000000) >> 24;
		buffer[7] = (m_seqno & 0x00FF0000) >> 16;
		buffer[8] = (m_seqno & 0x0000FF00) >> 8;
		buffer[9] = (m_seqno & 0x000000FF);
		buffer[10] = (command & 0xFF000000) >> 24;
		buffer[11] = (command & 0x00FF0000) >> 16;
		buffer[12] = (command & 0x0000FF00) >> 8;
		buffer[13] = (command & 0x000000FF);
		bufferpos += PROTOCOL_35_HEADER_SIZE;

		int payloadSize = (int)payload.length();
		int payload_len = GCM_IV_SIZE + payloadSize + GCM_TAG_SIZE;
		buffer[14] = (payload_len & 0xFF000000) >> 24;
		buffer[15] = (payload_len & 0x00FF0000) >> 16;
		buffer[16] = (payload_len & 0x0000FF00) >> 8;
		buffer[17] = (payload_len & 0x000000FF);

		memcpy(&buffer[bufferpos], iv, GCM_IV_SIZE);
		bufferpos += GCM_IV_SIZE;

		unsigned char *cEncryptedPayload = &buffer[bufferpos];
		int encryptedSize = 0;
		unsigned char tag[GCM_TAG_SIZE];

		if (aes_128_gcm_encrypt((unsigned char *)m_session_key, iv, GCM_IV_SIZE, &buffer[4],
		                        PROTOCOL_35_HEADER_SIZE - 4, (unsigned char *)payload.c_str(),
		                        payloadSize, cEncryptedPayload, &encryptedSize, tag, GCM_TAG_SIZE) != 0)
			return -1;

		bufferpos += encryptedSize;
		memcpy(&buffer[bufferpos], tag, GCM_TAG_SIZE);
		bufferpos += GCM_TAG_SIZE;

		buffer[bufferpos++] = (MESSAGE_SUFFIX & 0xFF000000) >> 24;
		buffer[bufferpos++] = (MESSAGE_SUFFIX & 0x00FF0000) >> 16;
		buffer[bufferpos++] = (MESSAGE_SUFFIX & 0x0000FF00) >> 8;
		buffer[bufferpos++] = (MESSAGE_SUFFIX & 0x000000FF);

		return bufferpos;
	}

	std::string DecodeTuyaMessage(unsigned char *buffer, const int size)
	{
		if (!m_session_established)
			return "{\"msg\":\"session not established\"}";

		std::string result;
		int bufferpos = 0;

		while (bufferpos < size) {
			unsigned char *cTuyaResponse = &buffer[bufferpos];
			if (bufferpos + PROTOCOL_35_HEADER_SIZE + MESSAGE_TRAILER_SIZE > size)
				break;

			int payload_len =
			    (int)((uint8_t)cTuyaResponse[17] + ((uint8_t)cTuyaResponse[16] << 8) +
			          ((uint8_t)cTuyaResponse[15] << 16) + ((uint8_t)cTuyaResponse[14] << 24));
			int messageSize = payload_len + PROTOCOL_35_HEADER_SIZE + MESSAGE_TRAILER_SIZE;
			if (bufferpos + messageSize > size)
				break;

			unsigned char iv[GCM_IV_SIZE];
			memcpy(iv, &cTuyaResponse[PROTOCOL_35_HEADER_SIZE], GCM_IV_SIZE);

			unsigned char tag[GCM_TAG_SIZE];
			memcpy(tag,
			       &cTuyaResponse[messageSize - MESSAGE_TRAILER_SIZE - GCM_TAG_SIZE],
			       GCM_TAG_SIZE);

			unsigned char *cEncryptedPayload =
			    &cTuyaResponse[PROTOCOL_35_HEADER_SIZE + GCM_IV_SIZE];
			int encryptedSize = payload_len - GCM_IV_SIZE - GCM_TAG_SIZE;

			std::vector<unsigned char> decrypted(encryptedSize + 16);
			int decryptedSize = 0;

			if (aes_128_gcm_decrypt((unsigned char *)m_session_key, iv, GCM_IV_SIZE, &cTuyaResponse[4],
			                        PROTOCOL_35_HEADER_SIZE - 4, cEncryptedPayload, encryptedSize,
			                        tag, GCM_TAG_SIZE, decrypted.data(), &decryptedSize) == 0) {
				int json_start = 0;
				if (decryptedSize >= 4 && decrypted[0] == 0 && decrypted[1] == 0) {
					int retcode = (int)((uint8_t)decrypted[3] + ((uint8_t)decrypted[2] << 8));
					if (retcode != 0) {
						result.append("{\"msg\":\"device returned error\"}");
						bufferpos += messageSize;
						continue;
					}
					json_start = 4;
				}

				for (int i = json_start; i < decryptedSize - 1; i++) {
					if (decrypted[i] == '{') {
						json_start = i;
						break;
					}
				}
				result.append((char *)decrypted.data() + json_start, decryptedSize - json_start);
			} else {
				result.append("{\"msg\":\"error decrypting payload\"}");
			}

			bufferpos += messageSize;
		}
		return result;
	}

	int BuildSessionMessage(unsigned char *buffer)
	{
		uint8_t command;
		std::string payload;

		if (m_seqno == 0) {
			m_seqno = 1;
			command = 3;
			payload = std::string((char *)m_local_nonce, 16);
		} else if (m_seqno == 1) {
			unsigned char rkey_hmac[32];
			hmac_sha256((unsigned char *)m_encryption_key.c_str(), m_encryption_key.length(),
			            m_remote_nonce, 16, rkey_hmac);

			m_seqno = 2;
			m_session_established = true;
			command = 5;
			payload = std::string((char *)rkey_hmac, 32);
		} else {
			return 0;
		}

		unsigned char iv[GCM_IV_SIZE];
		memcpy(iv, "0123456789ab", GCM_IV_SIZE);

		int bufferpos = 0;
		memset(buffer, 0, PROTOCOL_35_HEADER_SIZE);
		buffer[0] = (MESSAGE_PREFIX & 0xFF000000) >> 24;
		buffer[1] = (MESSAGE_PREFIX & 0x00FF0000) >> 16;
		buffer[2] = (MESSAGE_PREFIX & 0x0000FF00) >> 8;
		buffer[3] = (MESSAGE_PREFIX & 0x000000FF);
		buffer[6] = (m_seqno & 0xFF000000) >> 24;
		buffer[7] = (m_seqno & 0x00FF0000) >> 16;
		buffer[8] = (m_seqno & 0x0000FF00) >> 8;
		buffer[9] = (m_seqno & 0x000000FF);
		buffer[10] = (command & 0xFF000000) >> 24;
		buffer[11] = (command & 0x00FF0000) >> 16;
		buffer[12] = (command & 0x0000FF00) >> 8;
		buffer[13] = (command & 0x000000FF);
		bufferpos += PROTOCOL_35_HEADER_SIZE;

		int payloadSize = (int)payload.length();
		int payload_len = GCM_IV_SIZE + payloadSize + GCM_TAG_SIZE;
		buffer[14] = (payload_len & 0xFF000000) >> 24;
		buffer[15] = (payload_len & 0x00FF0000) >> 16;
		buffer[16] = (payload_len & 0x0000FF00) >> 8;
		buffer[17] = (payload_len & 0x000000FF);

		memcpy(&buffer[bufferpos], iv, GCM_IV_SIZE);
		bufferpos += GCM_IV_SIZE;

		unsigned char *cEncryptedPayload = &buffer[bufferpos];
		int encryptedSize = 0;
		unsigned char tag[GCM_TAG_SIZE];

		if (aes_128_gcm_encrypt((unsigned char *)m_encryption_key.c_str(), iv, GCM_IV_SIZE,
		                        &buffer[4], PROTOCOL_35_HEADER_SIZE - 4,
		                        (unsigned char *)payload.c_str(), payloadSize, cEncryptedPayload,
		                        &encryptedSize, tag, GCM_TAG_SIZE) != 0)
			return -1;

		bufferpos += encryptedSize;
		memcpy(&buffer[bufferpos], tag, GCM_TAG_SIZE);
		bufferpos += GCM_TAG_SIZE;

		buffer[bufferpos++] = (MESSAGE_SUFFIX & 0xFF000000) >> 24;
		buffer[bufferpos++] = (MESSAGE_SUFFIX & 0x00FF0000) >> 16;
		buffer[bufferpos++] = (MESSAGE_SUFFIX & 0x0000FF00) >> 8;
		buffer[bufferpos++] = (MESSAGE_SUFFIX & 0x000000FF);

		return bufferpos;
	}

	std::string DecodeSessionMessage(unsigned char *buffer, const int size)
	{
		if (size < PROTOCOL_35_HEADER_SIZE + MESSAGE_TRAILER_SIZE)
			return "";

		std::string result;
		unsigned char *cTuyaResponse = buffer;
		int payload_len =
		    (int)((uint8_t)cTuyaResponse[17] + ((uint8_t)cTuyaResponse[16] << 8) +
		          ((uint8_t)cTuyaResponse[15] << 16) + ((uint8_t)cTuyaResponse[14] << 24));

		unsigned char iv[GCM_IV_SIZE];
		memcpy(iv, &cTuyaResponse[PROTOCOL_35_HEADER_SIZE], GCM_IV_SIZE);

		unsigned char tag[GCM_TAG_SIZE];
		int messageSize = payload_len + PROTOCOL_35_HEADER_SIZE + MESSAGE_TRAILER_SIZE;
		memcpy(tag, &cTuyaResponse[messageSize - MESSAGE_TRAILER_SIZE - GCM_TAG_SIZE], GCM_TAG_SIZE);

		unsigned char *cEncryptedPayload = &cTuyaResponse[PROTOCOL_35_HEADER_SIZE + GCM_IV_SIZE];
		int encryptedSize = payload_len - GCM_IV_SIZE - GCM_TAG_SIZE;

		std::vector<unsigned char> decrypted(encryptedSize + 16);
		int decryptedSize = 0;

		if (aes_128_gcm_decrypt((unsigned char *)m_encryption_key.c_str(), iv, GCM_IV_SIZE,
		                        &cTuyaResponse[4], PROTOCOL_35_HEADER_SIZE - 4,
		                        cEncryptedPayload, encryptedSize, tag, GCM_TAG_SIZE, decrypted.data(),
		                        &decryptedSize) == 0) {
			int start = 0;
			if (decryptedSize >= 4 && decrypted[0] == 0 && decrypted[1] == 0)
				start = 4;

			result.append((char *)decrypted.data() + start, decryptedSize - start);
		} else {
			result.append("{\"msg\":\"error decrypting payload\"}");
		}

		if (m_seqno == 1 && result.length() >= 48) {
			memcpy(m_remote_nonce, result.c_str(), 16);

			unsigned char hmac_check[32];
			hmac_sha256((unsigned char *)m_encryption_key.c_str(), m_encryption_key.length(),
			            m_local_nonce, 16, hmac_check);

			if (memcmp(hmac_check, (unsigned char *)result.c_str() + 16, 32) != 0)
				return "";

			unsigned char xor_nonce[16];
			for (int i = 0; i < 16; i++)
				xor_nonce[i] = m_local_nonce[i] ^ m_remote_nonce[i];

			unsigned char iv2[GCM_IV_SIZE];
			memcpy(iv2, m_local_nonce, GCM_IV_SIZE);

			unsigned char ciphertext[32];
			int ciphertextSize = 0;
			unsigned char tag2[GCM_TAG_SIZE];

			if (aes_128_gcm_encrypt((unsigned char *)m_encryption_key.c_str(), iv2, GCM_IV_SIZE,
			                        nullptr, 0, xor_nonce, 16, ciphertext, &ciphertextSize, tag2,
			                        GCM_TAG_SIZE) != 0)
				return "";

			unsigned char full_output[44];
			memcpy(full_output, iv2, GCM_IV_SIZE);
			memcpy(full_output + GCM_IV_SIZE, ciphertext, 16);
			memcpy(full_output + GCM_IV_SIZE + 16, tag2, GCM_TAG_SIZE);
			memcpy(m_session_key, &full_output[12], 16);
		}

		return result;
	}

private:
	int aes_128_gcm_encrypt(const unsigned char *key, const unsigned char *iv, int iv_len,
	                        const unsigned char *aad, int aad_len, const unsigned char *input,
	                        int input_len, unsigned char *output, int *output_len, unsigned char *tag,
	                        int tag_len)
	{
		mbedtls_gcm_context ctx;
		mbedtls_gcm_init(&ctx);
		int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
		if (ret == 0) {
			ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, input_len, iv, iv_len, aad,
			                                aad_len, input, output, tag_len, tag);
		}
		mbedtls_gcm_free(&ctx);
		if (ret == 0)
			*output_len = input_len;
		return ret;
	}

	int aes_128_gcm_decrypt(const unsigned char *key, const unsigned char *iv, int iv_len,
	                        const unsigned char *aad, int aad_len, const unsigned char *input,
	                        int input_len, const unsigned char *tag, int tag_len, unsigned char *output,
	                        int *output_len)
	{
		mbedtls_gcm_context ctx;
		mbedtls_gcm_init(&ctx);
		int ret = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
		if (ret == 0) {
			ret = mbedtls_gcm_auth_decrypt(&ctx, input_len, iv, iv_len, aad, aad_len, tag, tag_len,
			                               input, output);
		}
		mbedtls_gcm_free(&ctx);
		if (ret == 0)
			*output_len = input_len;
		return ret;
	}

	void hmac_sha256(const unsigned char *key, int key_len, const unsigned char *data, int data_len,
	                 unsigned char *output)
	{
		const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
		mbedtls_md_context_t ctx;
		mbedtls_md_init(&ctx);
		if (md_info && mbedtls_md_setup(&ctx, md_info, 1) == 0) {
			mbedtls_md_hmac_starts(&ctx, key, key_len);
			mbedtls_md_hmac_update(&ctx, data, data_len);
			mbedtls_md_hmac_finish(&ctx, output);
		}
		mbedtls_md_free(&ctx);
	}

	void random_bytes(unsigned char *buffer, int len) { esp_fill_random(buffer, len); }

	bool m_session_established;
	uint32_t m_seqno;
	std::string m_encryption_key;
	unsigned char m_local_nonce[16];
	unsigned char m_remote_nonce[16];
	unsigned char m_session_key[16];
};

enum class State { DISCONNECTED, CONNECTING, NEGOTIATING, CONNECTED, DISCONNECTING };

static void append_dp_value(std::stringstream &ss, const DpCommand &cmd)
{
	switch (cmd.type) {
	case DpCommand::Type::BOOL:
		ss << (cmd.bool_val ? "true" : "false");
		break;
	case DpCommand::Type::INT:
		ss << cmd.int_val;
		break;
	case DpCommand::Type::STRING:
		ss << "\"" << cmd.str_val << "\"";
		break;
	}
}

constexpr const char *LOG_TAG = "tuya_client";

} // namespace

TuyaClient::TuyaClient(const TuyaDeviceConfig &config, QueueHandle_t cmd_queue)
    : cfg_(config), cmd_queue_(cmd_queue) {}

void TuyaClient::monitor_loop()
{
	const char *dev = cfg_.id.c_str();
	if (cfg_.version != "3.5") {
		ESP_LOGW(LOG_TAG, "[%s] Protocol %s requested, only 3.5 is implemented here", dev,
		         cfg_.version.c_str());
	}

	TuyaProtocol35 client;
	State state = State::DISCONNECTED;
	int sockfd = -1;
	unsigned char message_buffer[MAX_BUFFER_SIZE];
	time_t last_rx_time = 0;
	time_t state_start_time = 0;
	time_t last_connect_attempt = 0;
	bool dp_query_sent = false;

	while (true) {
		struct timeval tv = {0, 0};
		time_t now = time(nullptr);

		switch (state) {
		case State::DISCONNECTED:
			if (time(nullptr) - last_connect_attempt < 10)
				break;

			ESP_LOGI(LOG_TAG, "[%s] Connecting to %s:%d...", dev, cfg_.address.c_str(),
			         TUYA_COMMAND_PORT);
			last_connect_attempt = time(nullptr);

			client.SetEncryptionKey(cfg_.key);

			sockfd = socket(AF_INET, SOCK_STREAM, 0);
			if (sockfd < 0) {
				ESP_LOGE(LOG_TAG, "[%s] Failed to create socket: errno %d", dev, errno);
				break;
			}

			fcntl(sockfd, F_SETFL, O_NONBLOCK);

			{
				struct sockaddr_in addr;
				memset(&addr, 0, sizeof(addr));
				addr.sin_family = AF_INET;
				addr.sin_port = htons(TUYA_COMMAND_PORT);
				inet_pton(AF_INET, cfg_.address.c_str(), &addr.sin_addr);

				int err = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));
				if (err != 0 && errno != EINPROGRESS) {
					ESP_LOGE(LOG_TAG, "[%s] Connect failed: errno %d", dev, errno);
					state = State::DISCONNECTING;
					continue;
				}
			}
			state = State::CONNECTING;
			state_start_time = time(nullptr);
			dp_query_sent = false;
			break;

		case State::DISCONNECTING:
			if (sockfd >= 0)
				close(sockfd);
			sockfd = -1;
			state = State::DISCONNECTED;
			break;

		case State::CONNECTING: {
			if (time(nullptr) - state_start_time > 5) {
				ESP_LOGE(LOG_TAG, "[%s] Connection timeout", dev);
				state = State::DISCONNECTING;
				continue;
			}

			fd_set write_fds;
			FD_ZERO(&write_fds);
			FD_SET(sockfd, &write_fds);
			struct timeval tv_conn = {0, 0};
			int ret = select(sockfd + 1, nullptr, &write_fds, nullptr, &tv_conn);

			if (ret <= 0 || !FD_ISSET(sockfd, &write_fds))
				break;

			int error = 0;
			socklen_t len = sizeof(error);
			if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
				ESP_LOGE(LOG_TAG, "[%s] Connection failed: errno %d", dev, error);
				state = State::DISCONNECTING;
				continue;
			}

			ESP_LOGI(LOG_TAG, "[%s] Connected!", dev);

			int session_len = client.BuildSessionMessage(message_buffer);
			if (session_len < 0) {
				ESP_LOGE(LOG_TAG, "[%s] Failed to build session message", dev);
				state = State::DISCONNECTING;
				continue;
			}

			state = State::NEGOTIATING;
			state_start_time = time(nullptr);
			last_rx_time = time(nullptr);

			if (session_len > 0) {
				ssize_t sent = write(sockfd, message_buffer, session_len);
				if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
					ESP_LOGE(LOG_TAG, "[%s] Write error: errno %d", dev, errno);
					state = State::DISCONNECTING;
					continue;
				}
			}
			break;
		}

		case State::NEGOTIATING: {
			if (time(nullptr) - state_start_time > 5) {
				ESP_LOGE(LOG_TAG, "[%s] Negotiation timeout", dev);
				state = State::DISCONNECTING;
				continue;
			}

			if (!client.isSessionEstablished()) {
				ssize_t len = read(sockfd, message_buffer, sizeof(message_buffer));
				if (len > 0) {
					client.DecodeSessionMessage(message_buffer, len);

					if (client.isSessionEstablished()) {
						ESP_LOGI(LOG_TAG, "[%s] Negotiation complete", dev);
						state = State::CONNECTED;
						last_rx_time = time(nullptr);
						dp_query_sent = false;
					} else {
						unsigned char session_msg[MAX_BUFFER_SIZE];
						int session_len = client.BuildSessionMessage(session_msg);
						if (session_len < 0) {
							ESP_LOGE(LOG_TAG, "[%s] Negotiation failed", dev);
							state = State::DISCONNECTING;
							continue;
						} else if (session_len > 0) {
							ssize_t sent = write(sockfd, session_msg, session_len);
							if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
								ESP_LOGE(LOG_TAG, "[%s] Write error: errno %d", dev, errno);
								state = State::DISCONNECTING;
								continue;
							}
							if (client.isSessionEstablished()) {
								ESP_LOGI(LOG_TAG, "[%s] Negotiation complete", dev);
								state = State::CONNECTED;
								last_rx_time = time(nullptr);
								dp_query_sent = false;
							}
						}
					}
				} else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
					ESP_LOGE(LOG_TAG, "[%s] Read error: errno %d", dev, errno);
					state = State::DISCONNECTING;
					continue;
				}
			}

			// Wait for CONNECTED state to send DP query
			break;
		}

		case State::CONNECTED: {
			// Process any pending DP write commands
			if (cmd_queue_) {
				DpCommand cmd;
				while (xQueueReceive(cmd_queue_, &cmd, 0) == pdTRUE) {
					std::stringstream ss_payload;
					long currenttime = time(nullptr);
					ss_payload << "{\"protocol\":5,\"t\":" << currenttime << ",\"data\":{\"dps\":{\""
					           << cmd.dp << "\":";
					append_dp_value(ss_payload, cmd);
					ss_payload << "}}}";
					std::string payload = ss_payload.str();

					int len = client.BuildTuyaMessage(message_buffer, TUYA_CONTROL_NEW, payload);
					if (len > 0) {
						ssize_t sent = write(sockfd, message_buffer, len);
						if (sent == len) {
							ESP_LOGI(LOG_TAG, "[%s] Sent DP%d command", dev, cmd.dp);
						} else {
							ESP_LOGW(LOG_TAG, "[%s] Failed to send DP%d command", dev, cmd.dp);
						}
					}
				}
			}

			if (!dp_query_sent) {
				std::stringstream ss_payload;
				long currenttime = time(nullptr);
				ss_payload << "{\"gwId\":\"" << cfg_.id << "\",\"devId\":\"" << cfg_.id
				           << "\",\"uid\":\"" << cfg_.id << "\",\"t\":\"" << currenttime << "\"}";
				std::string payload = ss_payload.str();

				int len = client.BuildTuyaMessage(message_buffer, TUYA_DP_QUERY_NEW, payload);
				if (len > 0) {
					ssize_t sent = write(sockfd, message_buffer, len);
					if (sent == len) {
						ESP_LOGI(LOG_TAG, "[%s] Sent DP query, monitoring for updates...", dev);
						dp_query_sent = true;
					}
				}
			}

			ssize_t len = read(sockfd, message_buffer, sizeof(message_buffer));
			if (len > 0) {
				last_rx_time = time(nullptr);
				std::string decoded = client.DecodeTuyaMessage(message_buffer, len);
				if (!decoded.empty()) {
					ESP_LOGI(LOG_TAG, "[%s] Received: %s", dev, decoded.c_str());
				}
			} else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				ESP_LOGE(LOG_TAG, "[%s] Read error: errno %d", dev, errno);
				state = State::DISCONNECTING;
				continue;
			} else if (len == 0) {
				ESP_LOGW(LOG_TAG, "[%s] Connection closed by device", dev);
				state = State::DISCONNECTING;
				continue;
			}

			time_t now_hb = time(nullptr);
			if (now_hb - last_rx_time > 5) {
				int len = client.BuildTuyaMessage(message_buffer, TUYA_HEART_BEAT, "");
				if (len > 0) {
					ssize_t sent = write(sockfd, message_buffer, len);
					if (sent == len) {
						ESP_LOGI(LOG_TAG, "[%s] Sent heartbeat", dev);
						last_rx_time = now_hb;
					}
				}
			}
			break;
		}
		}

		switch (state) {
		case State::DISCONNECTED:
			tv.tv_sec = 10 - (now - last_connect_attempt);
			if (tv.tv_sec < 0)
				tv.tv_sec = 0;
			tv.tv_usec = 0;
			break;
		case State::CONNECTING:
			tv.tv_sec = 5 - (now - state_start_time);
			if (tv.tv_sec < 0)
				tv.tv_sec = 0;
			tv.tv_usec = 0;
			break;
		case State::NEGOTIATING:
			tv.tv_sec = 5 - (now - state_start_time);
			if (tv.tv_sec < 0)
				tv.tv_sec = 0;
			tv.tv_usec = 0;
			break;
		case State::CONNECTED:
			// Wake frequently to service button commands; keep heartbeats separate
			tv.tv_sec = 0;
			tv.tv_usec = 100 * 1000;  // 100ms
			break;
		case State::DISCONNECTING:
			tv.tv_sec = 0;
			tv.tv_usec = 0;
			break;
		}

		fd_set read_fds, write_fds;
		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);

		if (sockfd >= 0) {
			if (state == State::CONNECTING)
				FD_SET(sockfd, &write_fds);
			else
				FD_SET(sockfd, &read_fds);
		}

		select(sockfd + 1, &read_fds, &write_fds, nullptr, &tv);
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}
