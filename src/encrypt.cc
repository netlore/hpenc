/*
 * Copyright (c) 2014, Vsevolod Stakhov
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *	 * Redistributions of source code must retain the above copyright
 *	   notice, this list of conditions and the following disclaimer.
 *	 * Redistributions in binary form must reproduce the above copyright
 *	   notice, this list of conditions and the following disclaimer in the
 *	   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <cerrno>
#include <cstring>
#include <arpa/inet.h>
#include "encrypt.h"
#include "nonce.h"
#include "aead.h"
#include "util.h"
#include "kdf.h"
#include "thread_pool.h"
#include "aligned_alloc.h"

namespace hpenc
{

class HPEncEncrypt::impl {
public:
	std::unique_ptr<HPEncKDF> kdf;
	std::vector <std::shared_ptr<HPencAead> > ciphers;
	std::unique_ptr<HPEncNonce> nonce;
	int fd_in, fd_out;
	unsigned block_size;
	std::vector<std::shared_ptr<aligned_vector> > in_bufs;
	std::vector<std::shared_ptr<aligned_vector> > out_bufs;
	HPEncHeader hdr;
	bool encode;
	bool random_mode;
	std::unique_ptr<ThreadPool> pool;

	impl(std::unique_ptr<HPEncKDF> &&_kdf,
		const std::string &in,
		const std::string &out,
		AeadAlgorithm alg,
		unsigned _block_size,
		unsigned nthreads = 0,
		bool _rmode = false) : kdf(std::move(_kdf)), block_size(_block_size),
			hdr(alg, _block_size), random_mode(_rmode)
	{

		if (!in.empty()) {
			fd_in = open(in.c_str(), O_RDONLY);
			if (fd_in == -1) {
				std::cerr << "Cannot open input file '" << in << "': "
					<< ::strerror(errno) << std::endl;
			}
		}
		else {
			fd_in = STDIN_FILENO;
		}

		if (!out.empty()) {
			fd_out = open(out.c_str(), O_WRONLY | O_TRUNC);
			if (fd_out == -1) {
				std::cerr << "Cannot open output file '" << out << "': "
						<< ::strerror(errno) << std::endl;
			}
		}
		else {
			fd_out = STDOUT_FILENO;
		}

		encode = false;
		pool.reset(new ThreadPool(nthreads));
		in_bufs.resize(pool->size());
		out_bufs.resize(pool->size());
		hdr.nonce = kdf->initialNonce();
		auto klen = AeadKeyLengths[static_cast<int>(alg)];
		auto key = kdf->genKey(klen);

		for (auto i = 0U; i < pool->size(); i ++) {
			auto cipher = std::make_shared<HPencAead>(alg, _rmode);
			cipher->setKey(key);
			if (!nonce) {
				nonce.reset(new HPEncNonce(cipher->noncelen()));
			}
			ciphers.push_back(cipher);
			in_bufs[i] = std::make_shared<aligned_vector>();
			in_bufs[i]->resize(block_size);
			out_bufs[i] = std::make_shared<aligned_vector>();
			out_bufs[i]->resize(block_size + ciphers[0]->taglen());
		}
	}

	virtual ~impl()
	{
		if (fd_in != -1) close(fd_in);
		if (fd_out != -1) close(fd_out);
	}

	bool writeHeader()
	{
		return hdr.toFd(fd_out, encode);
	}

	ssize_t writeBlock(ssize_t rd, aligned_vector *in_buf, aligned_vector *out_buf,
			const std::vector<byte> &n, std::shared_ptr<HPencAead> const &cipher)
	{
		if (rd > 0) {
			auto bs = htonl(rd);
			auto tag = cipher->encrypt(reinterpret_cast<byte *>(&bs), sizeof(bs),
					n.data(), n.size(), in_buf->data(), rd, out_buf->data());

			if (!random_mode) {
				if (!tag) {
					return -1;
				}

				auto mac_pos = out_buf->data() + rd;
				std::copy(tag->data, tag->data + tag->datalen, mac_pos);
				return rd + tag->datalen;
			}

			return rd;

		}
		return -1;
	}

	size_t readBlock(aligned_vector *io_buf)
	{
		return util::atomicRead(fd_in, io_buf->data(), block_size);
	}
};

HPEncEncrypt::HPEncEncrypt(std::unique_ptr<HPEncKDF> &&kdf,
		const std::string &in,
		const std::string &out,
		AeadAlgorithm alg,
		unsigned block_size,
		unsigned nthreads,
		bool random_mode) :
	pimpl(new impl(std::move(kdf), in, out, alg, block_size, nthreads, random_mode))
{
}

HPEncEncrypt::~HPEncEncrypt()
{
}

void HPEncEncrypt::encrypt(bool encode, unsigned count)
{
	pimpl->encode = encode;
	bool last = false;
	auto remain = count;

	if (pimpl->random_mode || pimpl->writeHeader()) {
		auto nblocks = 0U;
		for (;;) {
			auto blocks_read = 0;
			std::vector< std::future<ssize_t> > results;
			auto i = 0U;
			for (auto &buf : pimpl->in_bufs) {
				if (count > 0) {
					if (remain == 0) {
						last = true;
						break;
					}
					remain --;
				}

				auto rd = pimpl->block_size;
				if (!pimpl->random_mode) {
					// For random mode we skip reading
					rd = pimpl->readBlock(buf.get());
				}

				if (rd > 0) {
					auto n = pimpl->nonce->incAndGet();
					results.emplace_back(
							pimpl->pool->enqueue(
								&impl::writeBlock, pimpl.get(), rd, buf.get(),
								pimpl->out_bufs[i].get(),
								n, pimpl->ciphers[i]
							));
					blocks_read ++;
				}
				else {
					last = true;
					break;
				}
				i ++;
			}

			i = 0;
			for(auto && result: results) {
				result.wait();
				auto rd = result.get();
				if (rd == -1) {
					throw std::runtime_error("Cannot encrypt block");
				}
				else {
					if (rd > 0) {
						const auto &io_buf = pimpl->out_bufs[i].get();
						if (encode) {
							auto b64_out = util::base64Encode(io_buf->data(), rd);
							if (util::atomicWrite(pimpl->fd_out,
									reinterpret_cast<const byte *>(b64_out.data()),
									b64_out.size()) == 0) {
								if (pimpl->random_mode) {
									// Assume that we are done
									std::cerr << "Cannot write output: " <<
											strerror(errno);
									return;
								}
								else {
									throw std::runtime_error("Cannot write "
											"encrypted block");
								}
							}
						}
						else {
							if (util::atomicWrite(pimpl->fd_out, io_buf->data(),
									rd) == 0) {
								if (pimpl->random_mode) {
									// Assume that we are done
									if (errno != ENOSPC && errno != EPIPE) {
										std::cerr << "Cannot write output: " <<
											strerror(errno) << std::endl;
									}
									return;
								}
								else {
									throw std::runtime_error("Cannot write "
											"encrypted block");
								}
							}
						}
					}
					if (rd < pimpl->block_size) {
						// We are done
						return;
					}
				}
				i++;
			}

			if (last) {
				return;
			}

			if (++nblocks % rekey_blocks == 0) {
				// Rekey all cipers
				auto nkey = pimpl->kdf->genKey(pimpl->ciphers[0]->keylen());
				for (auto const &cipher : pimpl->ciphers) {
					cipher->setKey(nkey);
				}
			}
		}
	}
}

} /* namespace hpenc */
