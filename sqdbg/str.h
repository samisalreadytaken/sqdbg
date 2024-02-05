//-----------------------------------------------------------------------
//                       github.com/samisalreadytaken/sqdbg
//-----------------------------------------------------------------------
//

#ifndef SQDBG_STRING_H
#define SQDBG_STRING_H

#include "debug.h"

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

#define STRLEN(s) (sizeof(s) - 1)

#ifdef _SQ64
	#define FMT_INT_LEN STRLEN("-9223372036854775808")
	#define FMT_PTR_LEN 18

	#if defined(_WIN32) || SQUIRREL_VERSION_NUMBER > 223
		#define FMT_INT "%lld"
		#define FMT_PTR "0x%016llX"
	#else
		#define FMT_INT "%ld"
		#define FMT_PTR "0x%016lX"
	#endif
#else
	#define FMT_INT_LEN STRLEN("-2147483648")
	#define FMT_PTR_LEN 10

	#define FMT_INT "%d"
	#define FMT_PTR "0x%08X"
#endif

#ifdef SQUNICODE
	#define FMT_STR "%ls"
	#define FMT_CSTR "%hs"
	#define FMT_VCSTR "%.*hs"
	#define FMT_VSTR "%.*ls"
#else
	#define FMT_STR "%s"
	#define FMT_CSTR "%s"
	#define FMT_VCSTR "%.*s"
	#define FMT_VSTR "%.*s"
#endif

#ifdef SQUSEDOUBLE
	#define FMT_FLT "%lf"
	#define FMT_FLT_LEN ( 1 + DBL_MAX_10_EXP + 1 + 1 + FLT_DIG )
#else
	#define FMT_FLT "%f"
	#define FMT_FLT_LEN ( 1 + FLT_MAX_10_EXP + 1 + 1 + FLT_DIG )
#endif


void *sqdbg_malloc( unsigned int size );
void sqdbg_free( void *p, unsigned int size );

template < typename C, typename I > int printint( C *buf, int size, I value );
template < typename C, typename I > int printhex( C *buf, int size, I value, bool padding = true );
template < int BUFSIZE > struct stringbuf_t;

#ifdef SQUNICODE
// expects terminated strings
inline int UTF8ToUnicode( wchar_t *dst, const char *src, int destSize )
{
	int ret = mbstowcs( dst, src, destSize );

	Assert( ret > 0 || !src[0] );

	if ( ret < 0 )
		ret = 0;

	return ret;
}

inline int UnicodeToUTF8( char *dst, const wchar_t *src, int destSize )
{
	int ret = wcstombs( dst, src, destSize );

	Assert( ret > 0 || !src[0] );

	if ( ret < 0 )
		ret = 0;

	return ret;
}

// Returns character length
inline int UnicodeLength( const char *src )
{
	return UTF8ToUnicode( NULL, src, 0 );
}

// Returns character/byte length
inline int UTF8Length( const wchar_t *src )
{
	return UnicodeToUTF8( NULL, src, 0 );
}
#endif

inline int scstombs( char *dst, int destSize, SQChar *src, int srcLen )
{
#ifdef SQUNICODE
	(void)srcLen;
	return UnicodeToUTF8( dst, src, destSize );
#else
	int len = min( destSize, srcLen );
	memcpy( dst, src, len );
	return len;
#endif
}

#define STR_EXPAND(s) s.len, s.ptr

struct string_t
{
	char *ptr;
	int len;

	string_t() {}

	string_t( const char *src, int size ) :
		ptr((char*)src),
		len(size)
	{}

	template < int BUFSIZE >
	string_t( stringbuf_t<BUFSIZE> &src ) :
		ptr(src.ptr),
		len(src.len)
	{}

#ifndef SQUNICODE
	string_t( SQString *src ) :
		ptr(src->_val),
		len(src->_len)
	{}
#endif

	template < int size >
	string_t( const char (&src)[size] ) :
		ptr((char*)src),
		len(size-1)
	{
		// input wasn't a string literal,
		// call ( src, size ) constructor instead
		Assert( (int)strlen(src) == len );
	}

	bool IsTerminated() const
	{
		return ( (int)strlen(ptr) == len );
	}

	template < int size >
	bool StartsWith( const char (&other)[size] ) const
	{
		if ( size-1 <= len )
			return !memcmp( ptr, other, size-1 );

		return false;
	}

	template < int size >
	bool IsEqualTo( const char (&other)[size] ) const
	{
		if ( size-1 == len )
			return !memcmp( ptr, other, size-1 );

		return false;
	}

	bool IsEqualTo( const char *other, int size ) const
	{
		if ( len == size )
			return !memcmp( ptr, other, size );

		return false;
	}

	bool IsEqualTo( const string_t &other ) const
	{
		if ( len == other.len )
			return !memcmp( ptr, other.ptr, len );

		return false;
	}

	bool IsEqualTo( const SQString *other ) const
	{
		if ( len == other->_len )
#ifdef SQUNICODE
		{
			Assert( len < 256 );
			char tmp[256];
			return !memcmp( ptr, tmp, UnicodeToUTF8( tmp, other->_val, sizeof(tmp) ) );
		}
#else
			return !memcmp( ptr, other->_val, len );
#endif
		return false;
	}

	bool IsEmpty() const
	{
		return !len || !ptr || !ptr[0];
	}

	bool Contains( char ch ) const
	{
		return ( memchr( ptr, ch, len ) != NULL );
	}

	template < int size >
	bool Contains( const char (&charset)[size] ) const
	{
		for ( const char *c = ptr + len - 1; c >= ptr; c-- )
		{
			if ( memchr( charset, *c, size-1 ) )
				return true;
		}
		return false;
	}

	void Copy( const string_t &src )
	{
		Copy( src.ptr, src.len );
	}

	void Copy( const char *src, int size )
	{
		Assert( !ptr );
		Assert( src );
		ptr = (char*)sqdbg_malloc( size + 1 );
		len = size;
		memcpy( ptr, src, size );
		ptr[size] = 0;
	}

	void FreeAndCopy( const string_t &src )
	{
		if ( ptr )
		{
			if ( len != src.len )
			{
				ptr = (char*)sqdbg_realloc( ptr, len + 1, src.len + 1 );
			}
		}
		else
		{
			ptr = (char*)sqdbg_malloc( src.len + 1 );
		}

		len = src.len;
		memcpy( ptr, src.ptr, len );
		ptr[len] = 0;
	}

	void Free()
	{
		if ( ptr )
		{
			sqdbg_free( ptr, len + 1 );
			ptr = NULL;
			len = 0;
		}
	}

	void Assign( const char *src )
	{
		Assert( src );
		ptr = (char*)src;
		len = strlen( src );
	}

	void Assign( const char *src, int size )
	{
		ptr = (char*)src;
		len = size;
	}

	void Assign( const string_t &src )
	{
		ptr = src.ptr;
		len = src.len;
	}

private:
	operator const char*();
	operator char*();
	string_t &operator=( const char *src );
};

#ifndef SQUNICODE
typedef string_t sqstring_t;
#else
struct sqstring_t
{
	SQChar *ptr;
	int len;

	sqstring_t() {}

	sqstring_t( const SQString *src ) :
		ptr((SQChar*)src->_val),
		len(src->_len)
	{}

	sqstring_t( const SQChar *src, int size ) :
		ptr((SQChar*)src),
		len(size)
	{}

	template < int size >
	sqstring_t( const SQChar (&src)[size] ) :
		ptr((SQChar*)src),
		len(size-1)
	{
		Assert( (int)scstrlen(src) == len );
	}

	bool IsEqualTo( const sqstring_t &other ) const
	{
		if ( len == other.len )
			return !memcmp( ptr, other.ptr, sq_rsl(len) );

		return false;
	}

	bool IsEqualTo( const SQString *other ) const
	{
		if ( len == other->_len )
			return !memcmp( ptr, other->_val, sq_rsl(len) );

		return false;
	}

	bool IsEmpty() const
	{
		return !len || !ptr || !ptr[0];
	}

	void Copy( const string_t &src )
	{
		Assert( !ptr );
		Assert( src.ptr );
		Assert( src.IsTerminated() );

		int l = UnicodeLength( src.ptr );

		ptr = (SQChar*)sqdbg_malloc( sq_rsl( l + 1 ) );
		len = l;
		UTF8ToUnicode( ptr, src.ptr, sq_rsl( l + 1 ) );
		ptr[len] = 0;
	}

	void Copy( const sqstring_t &src )
	{
		Assert( !ptr );
		Assert( src.ptr );

		ptr = (SQChar*)sqdbg_malloc( sq_rsl( src.len + 1 ) );
		len = src.len;
		memcpy( ptr, src.ptr, sq_rsl( len ) );
		ptr[len] = 0;
	}

	void FreeAndCopy( const sqstring_t &src )
	{
		if ( ptr )
		{
			if ( len != src.len )
			{
				ptr = (SQChar*)sqdbg_realloc( ptr, sq_rsl( len + 1 ), sq_rsl( src.len + 1 ) );
			}
		}
		else
		{
			ptr = (SQChar*)sqdbg_malloc( sq_rsl( src.len + 1 ) );
		}

		len = src.len;
		memcpy( ptr, src.ptr, sq_rsl( len ) );
		ptr[len] = 0;
	}

	void Free()
	{
		if ( ptr )
		{
			sqdbg_free( ptr, sq_rsl( len + 1 ) );
			ptr = NULL;
			len = 0;
		}
	}

	void Assign( const SQChar *src )
	{
		ptr = (SQChar*)src;
		len = scstrlen( src );
	}

	void Assign( const SQChar *src, int size )
	{
		ptr = (SQChar*)src;
		len = size;
	}

	void Assign( const SQString *src )
	{
		ptr = (SQChar*)src->_val;
		len = src->_len;
	}
};
#endif

template < int BUFSIZE >
struct stringbuf_t
{
	char ptr[BUFSIZE];
	int len;

	stringbuf_t() : len(0) {}

	int BytesLeft()
	{
		return BUFSIZE - len;
	}

	template < int size >
	void Puts( const char (&psz)[size] )
	{
		int amt = min( BytesLeft(), (size-1) );

		memcpy( ptr + len, psz, amt );
		len += amt;
	}

	void Puts( const string_t &str )
	{
		int amt = min( BytesLeft(), str.len );

		memcpy( ptr + len, str.ptr, amt );
		len += amt;
	}

#ifdef SQUNICODE
	void Puts( const sqstring_t &str )
	{
		len += UnicodeToUTF8( ptr + len, str.ptr, BytesLeft() );
	}
#endif

	void Put( char ch )
	{
		if ( BUFSIZE-1 >= len )
		{
			ptr[len++] = ch;
		}
	}

	void Term()
	{
		ptr[len] = 0;
		Assert( (int)strlen(ptr) == len );
	}

	template < typename I >
	void PutInt( I value )
	{
		int space = BytesLeft();

		if ( space < 2 )
			return;

		len += printint( ptr + len, space, value );
	}

	template < typename I >
	void PutHex( I value, bool padding = true )
	{
		int space = BytesLeft();

		if ( space < 2 )
			return;

		len += printhex( ptr + len, space, value, padding );
	}
};

template < typename I >
inline int countdigits( I input )
{
	int i = 0;

	do
	{
		input /= 10;
		i++;
	}
	while ( input );

	return i;
}

template < typename I >
inline int counthexdigits( I input )
{
	int i = 2;

	do
	{
		input >>= 4;
		i++;
	}
	while ( input );

	return i;
}

template < typename C, typename I >
inline int printint( C *buf, int size, I value )
{
	if ( !value )
	{
		if ( size >= 2 )
		{
			buf[0] = '0';
			buf[1] = 0;
			return 1;
		}

		return 0;
	}

	bool neg;
	int len;

	if ( value >= 0 )
	{
		len = countdigits( value );
		neg = false;
	}
	else
	{
		value = -value;
		len = countdigits( value ) + 1;
		buf[0] = '-';

		neg = ( value < 0 ); // value == INT_MIN
	}

	if ( len >= size )
		len = size-1;

	int i = len;

	buf[i--] = 0;

	do
	{
		C c = value % 10;
		value /= 10;
		buf[i--] = !neg ? ( '0' + c ) : ( '0' - c );
	}
	while ( value );

	return len;
}

template < typename C, typename I >
inline int printhex( C *buf, int size, I value, bool padding )
{
	int len = padding ? ( 2 + sizeof(I) * 2 ) : counthexdigits( value );

	if ( len >= size )
		len = size-1;

	int i = len;

	buf[i--] = 0;

	do
	{
		C c = value & 0xf;
		buf[i--] = c + ( ( c < 10 ) ? '0' : ( 'A' - 10 ) );
		value >>= 4;
	}
	while ( value );

	if ( padding )
	{
		for ( int pad = i - 1; pad--; )
			buf[i--] = '0';

		buf[i--] = 'x';
		buf[i] = '0';

		Assert( i == 0 );
		return len;
	}
	else
	{
		buf[0] = '0';
		buf[1] = 'x';
		i--;

		Assert( i == 0 );
		return len;
	}
}

template < typename I >
inline bool atoi( string_t str, I *out )
{
	Assert( str.ptr && str.len > 0 );

	I val = 0;
	bool neg = ( *str.ptr == '-' );

	if ( neg )
	{
		str.ptr++;
		str.len--;
	}

	for ( ; str.len--; str.ptr++ )
	{
		char ch = *str.ptr;

		if ( ch >= '0' && ch <= '9' )
		{
			val *= 10;
			val += ch - '0';
		}
		else
		{
			*out = 0;
			return false;
		}
	}

	*out = !neg ? val : -val;
	return true;
}

template < typename I >
inline bool atox( string_t str, I *out )
{
	if ( str.StartsWith("0x") )
	{
		str.ptr += 2;
		str.len -= 2;
	}

	I val = 0;

	for ( ; str.len--; str.ptr++ )
	{
		char ch = *str.ptr;

		if ( ch >= '0' && ch <= '9' )
		{
			val <<= 4;
			val += ch - '0';
		}
		else if ( ch >= 'A' && ch <= 'F' )
		{
			val <<= 4;
			val += ch - 'A' + 10;
		}
		else if ( ch >= 'a' && ch <= 'f' )
		{
			val <<= 4;
			val += ch - 'a' + 10;
		}
		else
		{
			*out = 0;
			return false;
		}
	}

	*out = val;
	return true;
}

#endif // SQDBG_STRING_H
