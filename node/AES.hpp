/*
 * Copyright (c)2013-2020 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2024-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#ifndef ZT_AES_HPP
#define ZT_AES_HPP

#include "Constants.hpp"
#include "Utils.hpp"
#include "SHA512.hpp"

#include <cstdint>
#include <cstring>

#ifndef ZT_AES_NO_ACCEL
#ifdef ZT_ARCH_X64
#include <wmmintrin.h>
#include <emmintrin.h>
#include <smmintrin.h>
#include <immintrin.h>
#define ZT_AES_AESNI 1
#endif
#endif

namespace ZeroTier {

/**
 * AES-256 and pals including GMAC, CTR, etc.
 *
 * This includes hardware acceleration for certain processors. The software
 * mode is fallback and is significantly slower.
 */
class AES
{
public:
	/**
	 * @return True if this system has hardware AES acceleration
	 */
	static ZT_INLINE bool accelerated()
	{
#ifdef ZT_AES_AESNI
		return Utils::CPUID.aes;
#else
		return false;
#endif
	}

	/**
	 * Create an un-initialized AES instance (must call init() before use)
	 */
	ZT_INLINE AES() noexcept {}

	/**
	 * Create an AES instance with the given key
	 *
	 * @param key 256-bit key
	 */
	explicit ZT_INLINE AES(const void *const key) noexcept { this->init(key); }

	ZT_INLINE ~AES() { Utils::burn(&_k,sizeof(_k)); }

	/**
	 * Set (or re-set) this AES256 cipher's key
	 *
	 * @param key 256-bit / 32-byte key
	 */
	ZT_INLINE void init(const void *key) noexcept
	{
#ifdef ZT_AES_AESNI
		if (likely(Utils::CPUID.aes)) {
			_init_aesni(reinterpret_cast<const uint8_t *>(key));
			return;
		}
#endif
		_initSW(reinterpret_cast<const uint8_t *>(key));
	}

	/**
	 * Encrypt a single AES block
	 *
	 * @param in Input block
	 * @param out Output block (can be same as input)
	 */
	ZT_INLINE void encrypt(const void *const in,void *const out) const noexcept
	{
#ifdef ZT_AES_AESNI
		if (likely(Utils::CPUID.aes)) {
			_encrypt_aesni(in,out);
			return;
		}
#endif
		_encryptSW(reinterpret_cast<const uint8_t *>(in),reinterpret_cast<uint8_t *>(out));
	}

	/**
	 * Decrypt a single AES block
	 *
	 * @param in Input block
	 * @param out Output block (can be same as input)
	 */
	ZT_INLINE void decrypt(const void *const in,void *const out) const noexcept
	{
#ifdef ZT_AES_AESNI
		if (likely(Utils::CPUID.aes)) {
			_decrypt_aesni(in,out);
			return;
		}
#endif
		_decryptSW(reinterpret_cast<const uint8_t *>(in),reinterpret_cast<uint8_t *>(out));
	}

	class GMACSIVEncryptor;

	/**
	 * Streaming GMAC calculator
	 */
	class GMAC
	{
		friend class GMACSIVEncryptor;

	public:
		/**
		 * Create a new instance of GMAC (must be initialized with init() before use)
		 *
		 * @param aes Keyed AES instance to use
		 */
		ZT_INLINE GMAC(const AES &aes) : _aes(aes) {}

		/**
		 * Reset and initialize for a new GMAC calculation
		 *
		 * @param iv 96-bit initialization vector (pad with zeroes if actual IV is shorter)
		 */
		ZT_INLINE void init(const uint8_t iv[12]) noexcept
		{
			_rp = 0;
			_len = 0;
			// We fill the least significant 32 bits in the _iv field with 1 since in GCM mode
			// this would hold the counter, but we're not doing GCM. The counter is therefore
			// always 1.
#ifdef ZT_AES_AESNI // also implies an x64 processor
			*reinterpret_cast<uint64_t *>(_iv) = *reinterpret_cast<const uint64_t *>(iv);
			*reinterpret_cast<uint32_t *>(_iv + 8) = *reinterpret_cast<const uint64_t *>(iv + 8);
			*reinterpret_cast<uint32_t *>(_iv + 12) = 0x01000000; // 0x00000001 in big-endian byte order
#else
			for(int i=0;i<12;++i)
				_iv[i] = iv[i];
			_iv[12] = 0;
			_iv[13] = 0;
			_iv[14] = 0;
			_iv[15] = 1;
#endif
			_y[0] = 0;
			_y[1] = 0;
		}

		/**
		 * Process data through GMAC
		 *
		 * @param data Bytes to process
		 * @param len Length of input
		 */
		void update(const void *data,unsigned int len) noexcept;

		/**
		 * Process any remaining cached bytes and generate tag
		 *
		 * Don't call finish() more than once or you'll get an invalid result.
		 *
		 * @param tag 128-bit GMAC tag (can be truncated)
		 */
		void finish(uint8_t tag[16]) noexcept;

	private:
		const AES &_aes;
		unsigned int _rp;
		unsigned int _len;
		uint8_t _r[16]; // remainder
		uint8_t _iv[16];
		uint64_t _y[2];
	};

	/**
	 * Streaming AES-CTR encrypt/decrypt
	 */
	class CTR
	{
		friend class GMACSIVEncryptor;

	public:
		ZT_INLINE CTR(const AES &aes) noexcept : _aes(aes) {}

		/**
		 * Initialize this CTR instance to encrypt a new stream
		 *
		 * @param iv Unique initialization vector
		 * @param output Buffer to which to store output (MUST be large enough for total bytes processed!)
		 */
		ZT_INLINE void init(const uint8_t iv[16],void *const output) noexcept
		{
			_ctr[0] = Utils::loadAsIsEndian<uint64_t>(iv);
			_ctr[1] = Utils::loadAsIsEndian<uint64_t>(iv + 8);
			_out = reinterpret_cast<uint8_t *>(output);
			_len = 0;
		}

		/**
		 * Encrypt or decrypt data, writing result to the output provided to init()
		 *
		 * @param input Input data
		 * @param len Length of input
		 */
		void crypt(const void *input,unsigned int len) noexcept;

		/**
		 * Finish any remaining bytes if total bytes processed wasn't a multiple of 16
		 *
		 * Don't call more than once for a given stream or data may be corrupted.
		 */
		void finish() noexcept;

	private:
		const AES &_aes;
		uint64_t _ctr[2];
		uint8_t *_out;
		unsigned int _len;
	};

	/**
	 * Encrypt with AES-GMAC-SIV
	 */
	class GMACSIVEncryptor
	{
	public:
		/**
		 * Create a new AES-GMAC-SIV encryptor keyed with the provided AES instances
		 *
		 * @param k0 First of two AES instances keyed with K0
		 * @param k1 Second of two AES instances keyed with K1
		 */
		ZT_INLINE GMACSIVEncryptor(const AES &k0,const AES &k1) noexcept :
			_gmac(k0),
			_ctr(k1) {}

		/**
		 * Initialize AES-GMAC-SIV
		 *
		 * @param iv IV in network byte order (byte order in which it will appear on the wire)
		 * @param output Pointer to buffer to receive ciphertext, must be large enough for all to-be-processed data!
		 */
		ZT_INLINE void init(const uint64_t iv,void *const output) noexcept
		{
			// Output buffer to receive the result of AES-CTR encryption.
			_output = output;

			// Initialize GMAC with 64-bit IV (and remaining 32 bits padded to zero).
			_iv[0] = iv;
			_iv[1] = 0;
			_gmac.init(reinterpret_cast<const uint8_t *>(_iv));
		}

		/**
		 * Process AAD (additional authenticated data) that is not being encrypted
		 *
		 * This must be called prior to update1, finish1, etc. if there is AAD to include
		 * in the MAC that is not included in the plaintext.
		 *
		 * @param aad Additional authenticated data
		 * @param len Length of AAD in bytes
		 */
		ZT_INLINE void aad(const void *const aad,unsigned int len) noexcept
		{
			// Feed ADD into GMAC first
			_gmac.update(aad,len);

			// End of AAD is padded to a multiple of 16 bytes to ensure unique encoding vs. plaintext.
			// AES-GCM-SIV does this as well for the same reason.
			len &= 0xfU;
			if (len != 0)
				_gmac.update(Utils::ZERO256,16 - len);
		}

		/**
		 * First pass plaintext input function
		 *
		 * @param input Plaintext chunk
		 * @param len Length of plaintext chunk
		 */
		ZT_INLINE void update1(const void *const input,const unsigned int len) noexcept
		{
			_gmac.update(input,len);
		}

		/**
		 * Finish first pass, compute CTR IV, initialize second pass.
		 */
		ZT_INLINE void finish1() noexcept
		{
			// Compute GMAC tag, then encrypt the original 64-bit IV and the first 64 bits
			// of the GMAC tag with AES (single block) and use this to initialize AES-CTR.
			uint64_t gmacTag[2];
			_gmac.finish(reinterpret_cast<uint8_t *>(gmacTag));
			_iv[1] = gmacTag[0];
			_ctr._aes.encrypt(_iv,_iv);

			// Bit 31 of the CTR IV is masked to (1) allow us to optimize by forgetting
			// about integer overflow for less than 2^31 bytes (which is far less than
			// this system's max message size), and (2) ensure interoperability with any
			// future FIPS-compliant or other cryptographic libraries that may or may not
			// handle 32-bit integer overflow of the least significant 32 bits in the
			// counter in the expected way.
			uint64_t ctrIv[2];
			ctrIv[0] = _iv[0];
			ctrIv[1] = _iv[1] & ZT_CONST_TO_BE_UINT64(0xffffffff7fffffffULL);
			_ctr.init(reinterpret_cast<const uint8_t *>(ctrIv),_output);
		}

		/**
		 * Second pass plaintext input function
		 *
		 * The same plaintext must be fed in the second time in the same order,
		 * though chunk boundaries do not have to be the same.
		 *
		 * @param input Plaintext chunk
		 * @param len Length of plaintext chunk
		 */
		ZT_INLINE void update2(const void *const input,const unsigned int len) noexcept
		{
			_ctr.crypt(input,len);
		}

		/**
		 * Finish second pass and return a pointer to the opaque 128-bit IV+MAC block
		 *
		 * @return Pointer to 128-bit opaque IV+MAC
		 */
		ZT_INLINE const uint8_t *finish2()
		{
			_ctr.finish();
			return reinterpret_cast<const uint8_t *>(_iv);
		}

	private:
		void *_output;
		uint64_t _iv[2];
		AES::GMAC _gmac;
		AES::CTR _ctr;
	};

private:
	static const uint32_t Te0[256];
	static const uint32_t Te1[256];
	static const uint32_t Te2[256];
	static const uint32_t Te3[256];
	static const uint32_t Te4[256];
	static const uint32_t Td0[256];
	static const uint32_t Td1[256];
	static const uint32_t Td2[256];
	static const uint32_t Td3[256];
	static const uint8_t Td4[256];
	static const uint32_t rcon[10];

	void _initSW(const uint8_t key[32]) noexcept;
	void _encryptSW(const uint8_t in[16],uint8_t out[16]) const noexcept;
	void _decryptSW(const uint8_t in[16],uint8_t out[16]) const noexcept;

	union {
#ifdef ZT_AES_AESNI
		struct {
			__m128i k[28];
			__m128i h,hh,hhh,hhhh;
		} ni;
#endif

		struct {
			uint64_t h[2];
			uint32_t ek[60];
			uint32_t dk[60];
		} sw;
	} _k;

#ifdef ZT_AES_AESNI
	static const __m128i s_shuf;

	void _init_aesni(const uint8_t key[32]) noexcept;

	ZT_INLINE void _encrypt_aesni(const void *const in,void *const out) const noexcept
	{
		__m128i tmp = _mm_loadu_si128((const __m128i *)in);
		tmp = _mm_xor_si128(tmp,_k.ni.k[0]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[1]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[2]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[3]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[4]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[5]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[6]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[7]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[8]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[9]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[10]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[11]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[12]);
		tmp = _mm_aesenc_si128(tmp,_k.ni.k[13]);
		_mm_storeu_si128((__m128i *)out,_mm_aesenclast_si128(tmp,_k.ni.k[14]));
	}

	ZT_INLINE void _decrypt_aesni(const void *in,void *out) const noexcept
	{
		__m128i tmp = _mm_loadu_si128((const __m128i *)in);
		tmp = _mm_xor_si128(tmp,_k.ni.k[14]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[15]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[16]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[17]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[18]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[19]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[20]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[21]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[22]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[23]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[24]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[25]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[26]);
		tmp = _mm_aesdec_si128(tmp,_k.ni.k[27]);
		_mm_storeu_si128((__m128i *)out,_mm_aesdeclast_si128(tmp,_k.ni.k[0]));
	}

	static ZT_INLINE __m128i _mult_block_aesni(const __m128i shuf,const __m128i h,__m128i y) noexcept
	{
		y = _mm_shuffle_epi8(y,shuf);
		__m128i t1 = _mm_clmulepi64_si128(h,y,0x00);
		__m128i t2 = _mm_clmulepi64_si128(h,y,0x01);
		__m128i t3 = _mm_clmulepi64_si128(h,y,0x10);
		__m128i t4 = _mm_clmulepi64_si128(h,y,0x11);
		t2 = _mm_xor_si128(t2,t3);
		t3 = _mm_slli_si128(t2,8);
		t2 = _mm_srli_si128(t2,8);
		t1 = _mm_xor_si128(t1,t3);
		t4 = _mm_xor_si128(t4,t2);
		__m128i t5 = _mm_srli_epi32(t1,31);
		t1 = _mm_slli_epi32(t1,1);
		__m128i t6 = _mm_srli_epi32(t4,31);
		t4 = _mm_slli_epi32(t4,1);
		t3 = _mm_srli_si128(t5,12);
		t6 = _mm_slli_si128(t6,4);
		t5 = _mm_slli_si128(t5,4);
		t1 = _mm_or_si128(t1,t5);
		t4 = _mm_or_si128(t4,t6);
		t4 = _mm_or_si128(t4,t3);
		t5 = _mm_slli_epi32(t1,31);
		t6 = _mm_slli_epi32(t1,30);
		t3 = _mm_slli_epi32(t1,25);
		t5 = _mm_xor_si128(t5,t6);
		t5 = _mm_xor_si128(t5,t3);
		t6 = _mm_srli_si128(t5,4);
		t4 = _mm_xor_si128(t4,t6);
		t5 = _mm_slli_si128(t5,12);
		t1 = _mm_xor_si128(t1,t5);
		t4 = _mm_xor_si128(t4,t1);
		t5 = _mm_srli_epi32(t1,1);
		t2 = _mm_srli_epi32(t1,2);
		t3 = _mm_srli_epi32(t1,7);
		t4 = _mm_xor_si128(t4,t2);
		t4 = _mm_xor_si128(t4,t3);
		t4 = _mm_xor_si128(t4,t5);
		return _mm_shuffle_epi8(t4,shuf);
	}
#endif
};

} // namespace ZeroTier

#endif
