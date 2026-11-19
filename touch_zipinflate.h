/* Bounded raw DEFLATE for a small, untrusted ZIP member. Included by fs.c,
 * after its dynamically loaded zlib declarations. */
qbool TouchZip_Inflate(const unsigned char *src, size_t n, unsigned char *dst, size_t outlen)
{
	z_stream s;
	int ret;
#ifndef LINK_TO_ZLIB
	if (!zlib_dll)
		return false;
#endif
	if (n > 0xffffffffu || outlen > 0xffffffffu)
		return false;
	memset(&s, 0, sizeof(s));
	if (qz_inflateInit2(&s, -MAX_WBITS) != Z_OK)
		return false;
	s.next_in = (unsigned char *)src;
	s.avail_in = (unsigned int)n;
	s.next_out = dst;
	s.avail_out = (unsigned int)outlen;
	ret = qz_inflate(&s, Z_FINISH);
	qz_inflateEnd(&s);
	return ret == Z_STREAM_END && s.total_out == outlen && s.total_in == n;
}
