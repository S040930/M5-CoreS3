#include <string.h>

#include "audio_crypto.h"

#include "esp_log.h"
#include "mbedtls/aes.h"
#include "sodium.h"

static const char *TAG = "audio_crypto";

static int chacha20_decrypt(const uint8_t *key, const uint8_t *packet,
                            size_t packet_len, const uint8_t *ciphertext,
                            size_t ciphertext_len, uint8_t *output) {
  uint8_t nonce[12] = {0};
  memcpy(nonce + 4, packet + packet_len - 8, 8);
  const uint8_t *aad = packet + 4;
  size_t aad_len = 8;

  unsigned long long decrypted_len = 0;
  int ret = crypto_aead_chacha20poly1305_ietf_decrypt(
      output, &decrypted_len, NULL, ciphertext, ciphertext_len, aad, aad_len,
      nonce, key);
  if (ret != 0) {
    return -1;
  }
  return (int)decrypted_len;
}

int audio_crypto_decrypt_rtp(const audio_encrypt_t *encrypt,
                             const uint8_t *input, size_t input_len,
                             uint8_t *output, size_t output_capacity,
                             const uint8_t *full_packet,
                             size_t full_packet_len) {
  if (!encrypt || !input || !output) {
    return -1;
  }

  if (encrypt->type == AUDIO_ENCRYPT_NONE) {
    if (input_len > output_capacity) {
      return -1;
    }
    memcpy(output, input, input_len);
    return (int)input_len;
  }

  if (encrypt->type == AUDIO_ENCRYPT_AES_CBC) {
    if (encrypt->key_len != 16) {
      ESP_LOGE(TAG, "Invalid AES-CBC key length: %zu (expected 16)",
               encrypt->key_len);
      return -1;
    }
    if (input_len > output_capacity) {
      return -1;
    }

    uint8_t iv[16];
    memcpy(iv, encrypt->iv, sizeof(iv));

    size_t num_blocks = input_len / 16;
    size_t remainder = input_len % 16;
    size_t encrypted_len = num_blocks * 16;

    if (encrypted_len > 0) {
      mbedtls_aes_context aes;
      mbedtls_aes_init(&aes);

      int ret = mbedtls_aes_setkey_dec(&aes, encrypt->key, 128);
      if (ret != 0) {
        mbedtls_aes_free(&aes);
        return -1;
      }

      ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, encrypted_len, iv,
                                  input, output);
      mbedtls_aes_free(&aes);
      if (ret != 0) {
        return -1;
      }
    }

    if (remainder > 0) {
      memcpy(output + encrypted_len, input + encrypted_len, remainder);
    }

    return (int)input_len;
  }

  if (encrypt->type == AUDIO_ENCRYPT_CHACHA20_POLY1305) {
    if (!full_packet || full_packet_len < 12) {
      return -1;
    }

    if (input_len < crypto_aead_chacha20poly1305_ietf_ABYTES + 8) {
      return -1;
    }

    size_t ciphertext_len = input_len - 8;
    return chacha20_decrypt(encrypt->key, full_packet, full_packet_len, input,
                            ciphertext_len, output);
  }

  return -1;
}

int audio_crypto_decrypt_buffered(const audio_encrypt_t *encrypt,
                                  const uint8_t *packet, size_t packet_len,
                                  uint8_t *output, size_t output_capacity) {
  if (!packet || !output) {
    return -1;
  }

  if (!encrypt || encrypt->type != AUDIO_ENCRYPT_CHACHA20_POLY1305) {
    if (packet_len <= 12) {
      return -1;
    }
    size_t payload_len = packet_len - 12;
    if (payload_len > output_capacity) {
      return -1;
    }
    memcpy(output, packet + 12, payload_len);
    return (int)payload_len;
  }

  if (packet_len < 36) {
    return -1;
  }

  const uint8_t *ciphertext = packet + 12;
  size_t ciphertext_len = packet_len - 12 - 8;

  if (ciphertext_len >
      output_capacity + crypto_aead_chacha20poly1305_ietf_ABYTES) {
    return -1;
  }

  return chacha20_decrypt(encrypt->key, packet, packet_len, ciphertext,
                          ciphertext_len, output);
}
