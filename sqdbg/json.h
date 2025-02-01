//-----------------------------------------------------------------------
//                       github.com/samisalreadytaken/sqdbg
//-----------------------------------------------------------------------
//

#ifndef SQDBG_JSON_H
#define SQDBG_JSON_H

// Most messages are going to require less than 256 bytes,
// only approaching 1024 on large breakpoint requests
#define JSON_SCRATCH_CHUNK_SIZE 1024

typedef enum
{
	JSON_NULL			= 0x0000,
	JSON_BOOL			= 0x0001,
	JSON_INTEGER		= 0x0002,
	JSON_FLOAT			= 0x0004,
	JSON_STRING			= 0x0008,
	JSON_TABLE			= 0x0010,
	JSON_ARRAY			= 0x0020,
} JSONTYPE;

class json_table_t;
class json_array_t;
struct json_value_t;

struct ostr_t
{
	// ushort can handle strings that fit the recv net buf,
	// use a larger type for validation of sent messages in the send net buf
#ifdef SQDBG_VALIDATE_SENT_MSG
	typedef unsigned int index_t;
#else
	typedef unsigned short index_t;
#endif

	index_t ofs;
	index_t len;
};

struct json_value_t
{
	union
	{
		ostr_t _string;
		int _integer;
		json_table_t *_table;
		json_array_t *_array;
	};

	int type;
};

struct json_field_t
{
	ostr_t key;
	json_value_t val;
};

class json_array_t
{
public:
	const char *m_pBase;
	CScratch< JSON_SCRATCH_CHUNK_SIZE > *m_Allocator;
	int *m_Elements;
	unsigned short m_nElementCount;
	unsigned short m_nElementsSize;

	void Init( const char *base, CScratch< JSON_SCRATCH_CHUNK_SIZE > *allocator )
	{
		m_pBase = base;
		m_Allocator = allocator;
		m_nElementCount = 0;
		m_nElementsSize = 0;
	}

	json_value_t *NewElement()
	{
		if ( m_nElementCount == m_nElementsSize )
		{
			// doesn't free old ptr, this is an uncommon operation and extra allocation is fine
			int oldsize = m_nElementsSize;
			int *oldptr = m_Elements;

			m_nElementsSize = !m_nElementsSize ? 8 : ( m_nElementsSize << 1 );
			m_Elements = (int*)m_Allocator->Alloc( m_nElementsSize * sizeof(int) );

			if ( oldsize )
				memcpy( m_Elements, oldptr, oldsize * sizeof(int) );
		}

		int index;
		json_value_t *ret = (json_value_t*)m_Allocator->Alloc( sizeof(json_value_t), &index );
		m_Elements[ m_nElementCount++ ] = index;
		return ret;
	}

	int size() const
	{
		return m_nElementCount;
	}

	bool GetString( int i, string_t *out )
	{
		Assert( m_nElementCount );
		Assert( i >= 0 && i < m_nElementCount );

		json_value_t *val = (json_value_t*)m_Allocator->Get( m_Elements[i] );

		if ( val->type & JSON_STRING )
		{
			out->Assign( m_pBase + val->_string.ofs, val->_string.len );
			return true;
		}

		return false;
	}

	bool GetTable( int i, json_table_t **out )
	{
		Assert( m_nElementCount );
		Assert( i >= 0 && i < m_nElementCount );

		json_value_t *val = (json_value_t*)m_Allocator->Get( m_Elements[i] );

		if ( val->type & JSON_TABLE )
		{
			*out = val->_table;
			return true;
		}

		return false;
	}
};

class json_table_t
{
public:
	const char *m_pBase;
	CScratch< JSON_SCRATCH_CHUNK_SIZE > *m_Allocator;
	int *m_Elements;
	unsigned short m_nElementCount;
	unsigned short m_nElementsSize;

	void Init( const char *base, CScratch< JSON_SCRATCH_CHUNK_SIZE > *allocator )
	{
		m_pBase = base;
		m_Allocator = allocator;
		m_nElementCount = 0;
		m_nElementsSize = 0;
	}

	json_value_t *Get( const string_t &key )
	{
		for ( int i = 0; i < m_nElementCount; i++ )
		{
			json_field_t *kv = (json_field_t*)m_Allocator->Get( m_Elements[i] );

			if ( key.IsEqualTo( m_pBase + kv->key.ofs, kv->key.len ) )
				return &kv->val;
		}

		return NULL;
	}

	json_value_t *Get( const string_t &key ) const
	{
		return const_cast< json_table_t * >( this )->Get( key );
	}

	json_field_t *NewElement()
	{
		if ( m_nElementCount == m_nElementsSize )
		{
			int oldsize = m_nElementsSize;
			int *oldptr = m_Elements;

			m_nElementsSize = !m_nElementsSize ? 8 : ( m_nElementsSize << 1 );
			m_Elements = (int*)m_Allocator->Alloc( m_nElementsSize * sizeof(int) );

			if ( oldsize )
				memcpy( m_Elements, oldptr, oldsize * sizeof(int) );
		}

		int index;
		json_field_t *ret = (json_field_t*)m_Allocator->Alloc( sizeof(json_field_t), &index );
		m_Elements[ m_nElementCount++ ] = index;
		return ret;
	}

	bool GetBool( const string_t &key, bool *out ) const
	{
		json_value_t *kval = Get( key );

		if ( kval && ( kval->type & JSON_BOOL ) )
		{
			*out = kval->_integer;
			return true;
		}

		*out = false;
		return false;
	}

	bool GetInt( const string_t &key, int *out, int defaultVal = 0 ) const
	{
		json_value_t *kval = Get( key );

		if ( kval && ( kval->type & JSON_INTEGER ) )
		{
			*out = kval->_integer;
			return true;
		}

		*out = defaultVal;
		return false;
	}

	bool GetString( const string_t &key, string_t *out, const char *defaultVal = "" ) const
	{
		json_value_t *kval = Get( key );

		if ( kval && ( kval->type & JSON_STRING ) )
		{
			out->Assign( m_pBase + kval->_string.ofs, kval->_string.len );
			return true;
		}

		out->Assign( defaultVal, strlen(defaultVal) );
		return false;
	}

	bool GetTable( const string_t &key, json_table_t **out ) const
	{
		json_value_t *kval = Get( key );

		if ( kval && ( kval->type & JSON_TABLE ) )
		{
			*out = kval->_table;
			return true;
		}

		*out = NULL;
		return false;
	}

	bool GetArray( const string_t &key, json_array_t **out ) const
	{
		json_value_t *kval = Get( key );

		if ( kval && ( kval->type & JSON_ARRAY ) )
		{
			*out = kval->_array;
			return true;
		}

		*out = NULL;
		return false;
	}

	bool Get( const string_t &key, bool *out ) const { return GetBool( key, out ); }
	bool Get( const string_t &key, int *out ) const { return GetInt( key, out ); }
	bool Get( const string_t &key, string_t *out ) const { return GetString( key, out ); }
	bool Get( const string_t &key, json_table_t **out ) const { return GetTable( key, out ); }
	bool Get( const string_t &key, json_array_t **out ) const { return GetArray( key, out ); }
};

static inline void PutStrL( CBuffer *buffer, const string_t &str )
{
	buffer->_base.Ensure( buffer->size() + str.len );
	memcpy( buffer->base() + buffer->size(), str.ptr, str.len );
	buffer->_size += str.len;
}

static inline void PutStr( CBuffer *buffer, const string_t &str, bool quote )
{
	const char *c = str.ptr;
	unsigned int i = str.len;

	unsigned int len = 2 + i;

	if ( quote )
		len += 4;

	for ( ; i--; c++ )
	{
		switch ( *c )
		{
			case '\"': case '\\': case '\b':
			case '\f': case '\n': case '\r': case '\t':
				len++;
				if ( quote )
				{
					len++;
					if ( *c == '\"' || *c == '\\' )
						len++;
				}
				break;
			default:
				if ( !IN_RANGE_CHAR( *(unsigned char*)c, 0x20, 0x7E ) )
				{
					int ret = IsValidUTF8( (unsigned char*)c, i + 1 );
					if ( ret != 0 )
					{
						i -= ret - 1;
						c += ret - 1;
					}
					else
					{
						if ( !quote )
						{
							len += sizeof(uint16_t) * 2 + 1;
						}
						else
						{
							len += sizeof(SQChar) * 2 + 2;
						}
					}
				}
		}
	}

	buffer->_base.Ensure( buffer->size() + len );

	char *mem = buffer->base();
	unsigned int idx = buffer->size();

	c = str.ptr;
	i = str.len;

	mem[idx++] = '\"';

	if ( quote )
	{
		mem[idx++] = '\\';
		mem[idx++] = '\"';
	}

	for ( ; i--; c++ )
	{
		mem[idx++] = *c;

		switch ( *c )
		{
			case '\"':
				mem[idx-1] = '\\';
				if ( quote )
				{
					mem[idx++] = '\\';
					mem[idx++] = '\\';
				}
				mem[idx++] = '\"';
				break;
			case '\\':
				if ( quote )
				{
					mem[idx++] = '\\';
					mem[idx++] = '\\';
				}
				mem[idx++] = '\\';
				break;
			case '\b':
				mem[idx-1] = '\\';
				if ( quote )
					mem[idx++] = '\\';
				mem[idx++] = 'b';
				break;
			case '\f':
				mem[idx-1] = '\\';
				if ( quote )
					mem[idx++] = '\\';
				mem[idx++] = 'f';
				break;
			case '\n':
				mem[idx-1] = '\\';
				if ( quote )
					mem[idx++] = '\\';
				mem[idx++] = 'n';
				break;
			case '\r':
				mem[idx-1] = '\\';
				if ( quote )
					mem[idx++] = '\\';
				mem[idx++] = 'r';
				break;
			case '\t':
				mem[idx-1] = '\\';
				if ( quote )
					mem[idx++] = '\\';
				mem[idx++] = 't';
				break;
			default:
				if ( !IN_RANGE_CHAR( *(unsigned char*)c, 0x20, 0x7E ) )
				{
					int ret = IsValidUTF8( (unsigned char*)c, i + 1 );
					if ( ret != 0 )
					{
						memcpy( mem + idx, c + 1, ret - 1 );
						idx += ret - 1;
						i -= ret - 1;
						c += ret - 1;
					}
					else
					{
						mem[idx-1] = '\\';

						if ( !quote )
						{
							mem[idx++] = 'u';
							idx += printhex< true, false >(
									mem + idx,
									buffer->capacity() - idx,
									(uint16_t)*(unsigned char*)c );
						}
						else
						{
							mem[idx++] = '\\';
							mem[idx++] = 'x';
							idx += printhex< true, false >(
									mem + idx,
									buffer->capacity() - idx,
									(SQChar)*(unsigned char*)c );
						}
					}
				}
		}
	}

	if ( quote )
	{
		mem[idx++] = '\\';
		mem[idx++] = '\"';
	}

	mem[idx++] = '\"';

	buffer->_size = idx;
}

#ifdef SQUNICODE
static inline void PutStr( CBuffer *buffer, const sqstring_t &str, bool quote )
{
	unsigned int len;

	if ( !quote )
	{
		len = 2 + UTF8Length< kUTFEscapeJSON >( str.ptr, str.len );
	}
	else
	{
		len = 2 + UTF8Length< kUTFEscapeQuoted >( str.ptr, str.len );
	}

	buffer->_base.Ensure( buffer->size() + len );
	buffer->base()[buffer->_size++] = '\"';

	if ( !quote )
	{
		len = SQUnicodeToUTF8< kUTFEscapeJSON >(
				buffer->base() + buffer->size(),
				buffer->capacity() - buffer->size(),
				str.ptr,
				str.len );
	}
	else
	{
		len = SQUnicodeToUTF8< kUTFEscapeQuoted >(
				buffer->base() + buffer->size(),
				buffer->capacity() - buffer->size(),
				str.ptr,
				str.len );
	}

	buffer->_size += len;
	buffer->base()[buffer->_size++] = '\"';
}
#endif

static inline void PutChar( CBuffer *buffer, char c )
{
	buffer->_base.Ensure( buffer->size() + 1 );
	buffer->base()[buffer->_size++] = c;
}

template < typename I >
static inline void PutInt( CBuffer *buffer, I val, bool hex = false )
{
	int len;
	buffer->_base.Ensure( buffer->size() + countdigits( val ) + 1 );

	if ( !hex )
	{
		len = printint( buffer->base() + buffer->size(), buffer->capacity() - buffer->size(), val );
	}
	else
	{
		len = printhex< false >( buffer->base() + buffer->size(), buffer->capacity() - buffer->size(), val );
	}

	buffer->_size += len;
}

class wjson_table_t;
class wjson_array_t;

class wjson_t
{
public:
	CBuffer *m_pBuffer;
	int m_nElementCount;

	wjson_t( CBuffer *b ) :
		m_pBuffer(b),
		m_nElementCount(0)
	{
	}
};

class wjson_table_t : public wjson_t
{
public:
	wjson_table_t( CBuffer &b ) : wjson_t(&b)
	{
		PutChar( m_pBuffer, '{' );
	}

	~wjson_table_t()
	{
		PutChar( m_pBuffer, '}' );
	}

	wjson_table_t( const wjson_t &src ) : wjson_t(src)
	{
	}

	wjson_table_t( const wjson_table_t &src );

	void PutKey( const string_t &key )
	{
		if ( m_nElementCount++ )
			PutChar( m_pBuffer, ',' );

		PutChar( m_pBuffer, '\"' );
		PutStrL( m_pBuffer, key );
		PutChar( m_pBuffer, '\"' );
		PutChar( m_pBuffer, ':' );
	}

	void SetInt( const string_t &key, int val )
	{
		PutKey( key );
		PutInt( m_pBuffer, val );
	}

	void SetNull( const string_t &key )
	{
		PutKey( key );
		PutStrL( m_pBuffer, "null" );
	}

	void SetBool( const string_t &key, bool val )
	{
		PutKey( key );
		PutStrL( m_pBuffer, val ? string_t("true") : string_t("false") );
	}

	void SetString( const string_t &key, const string_t &val, bool quote = false )
	{
		PutKey( key );
		PutStr( m_pBuffer, val, quote );
	}

#ifdef SQUNICODE
	void SetString( const string_t &key, const sqstring_t &val, bool quote = false )
	{
		PutKey( key );
		PutStr( m_pBuffer, val, quote );
	}
#endif

	void SetIntString( const string_t &key, int val )
	{
		PutKey( key );
		PutChar( m_pBuffer, '\"' );
		PutInt( m_pBuffer, val );
		PutChar( m_pBuffer, '\"' );
	}

	template < typename I >
	void SetIntBrackets( const string_t &key, I val, bool hex = false )
	{
		PutKey( key );
		PutChar( m_pBuffer, '\"' );
		PutChar( m_pBuffer, '[' );
		PutInt( m_pBuffer, val, hex );
		PutChar( m_pBuffer, ']' );
		PutChar( m_pBuffer, '\"' );
	}

	wjson_t SetArray( const string_t &key )
	{
		PutKey( key );
		PutChar( m_pBuffer, '[' );
		return { m_pBuffer };
	}

	wjson_t SetTable( const string_t &key )
	{
		PutKey( key );
		PutChar( m_pBuffer, '{' );
		return { m_pBuffer };
	}

	void Set( const string_t &key, bool val ) { SetBool( key, val ); }
	void Set( const string_t &key, int val ) { SetInt( key, val ); }
	void Set( const string_t &key, unsigned int val ) { SetInt( key, val ); }
	void Set( const string_t &key, const string_t &val ) { SetString( key, val ); }
};

class wjson_array_t : public wjson_t
{
public:
	wjson_array_t( CBuffer &b ) : wjson_t(&b)
	{
		PutChar( m_pBuffer, '[' );
	}

	~wjson_array_t()
	{
		PutChar( m_pBuffer, ']' );
	}

	wjson_array_t( const wjson_t &src ) : wjson_t(src)
	{
	}

	wjson_array_t( const wjson_array_t &src );

	int size()
	{
		return m_nElementCount;
	}

	wjson_t AppendTable()
	{
		if ( m_nElementCount++ )
			PutChar( m_pBuffer, ',' );

		PutChar( m_pBuffer, '{' );
		return { m_pBuffer };
	}

	void Append( int val )
	{
		if ( m_nElementCount++ )
			PutChar( m_pBuffer, ',' );

		PutInt( m_pBuffer, val );
	}

	void Append( const string_t &val )
	{
		if ( m_nElementCount++ )
			PutChar( m_pBuffer, ',' );

		PutChar( m_pBuffer, '\"' );
		PutStrL( m_pBuffer, val );
		PutChar( m_pBuffer, '\"' );
	}
};

class JSONParser
{
private:
	char *m_cur;
	char *m_end;
	char *m_start;
	CScratch< JSON_SCRATCH_CHUNK_SIZE > *m_Allocator;
	char *m_error;

	enum
	{
		Token_Error = 0,
		Token_String,
		Token_Integer,
		Token_Float,
		Token_False,
		Token_True = Token_False + 1,
		Token_Null,
		Token_Table = '{',
		Token_Array = '[',
	};

public:
	JSONParser( CScratch< JSON_SCRATCH_CHUNK_SIZE > *allocator, char *ptr, int len, json_table_t *pTable ) :
		m_cur( ptr ),
		m_end( ptr + len + 1 ),
		m_start( ptr ),
		m_Allocator( allocator ),
		m_error( NULL )
	{
		string_t token;
		char type = NextToken( token );

		if ( type == '{' )
		{
			pTable->Init( m_start, m_Allocator );
			ParseTable( pTable, token );
		}
		else
		{
			SetError( "expected '%c', got '0x%02x' @ %i", '{', Char(type), Index() );
		}
	}

	const char *GetError() const
	{
		return m_error;
	}

private:
	int Index()
	{
		return m_cur - m_start;
	}

	unsigned char Char( char token )
	{
		if ( token != Token_Error )
			return (unsigned char)token;

		return *(unsigned char*)m_cur;
	}

	void SetError( const char *fmt, ... )
	{
		if ( m_error )
			return;

		const int size = 48;
		m_error = m_Allocator->Alloc( size );

		va_list va;
		va_start( va, fmt );
		int len = vsnprintf( m_error, size, fmt, va );
		va_end( va );

		if ( len < 0 || len > size-1 )
			len = size-1;

		m_error[len] = 0;
	}

	char NextToken( string_t &token )
	{
		while ( m_cur < m_end )
		{
			switch ( *m_cur )
			{
				case 0x20: case 0x0A: case 0x0D: case 0x09:
					m_cur++;
					break;

				case '\"':
					return ParseString( token );

				case '-':
				case '0': case '1': case '2': case '3': case '4':
				case '5': case '6': case '7': case '8': case '9':
					return ParseNumber( token );

				case ':': case ',':
				case '{': case '}':
				case '[': case ']':
					return *m_cur++;

				case 't':
					if ( m_cur + 4 >= m_end ||
							m_cur[1] != 'r' || m_cur[2] != 'u' || m_cur[3] != 'e' )
					{
						SetError( "expected %s @ %i", "\"true\"", Index() );
						return Token_Error;
					}

					m_cur += 4;
					return Token_True;

				case 'f':
					if ( m_cur + 5 >= m_end ||
							m_cur[1] != 'a' || m_cur[2] != 'l' || m_cur[3] != 's' || m_cur[4] != 'e' )
					{
						SetError( "expected %s @ %i", "\"false\"", Index() );
						return Token_Error;
					}

					m_cur += 5;
					return Token_False;

				case 'n':
					if ( m_cur + 4 >= m_end ||
							m_cur[1] != 'u' || m_cur[2] != 'l' || m_cur[3] != 'l' )
					{
						SetError( "expected %s @ %i", "\"null\"", Index() );
						return Token_Error;
					}

					m_cur += 4;
					return Token_Null;

				default:
					return Token_Error;
			}
		}

		return Token_Error;
	}

	char ParseString( string_t &token )
	{
		char *pStart = ++m_cur;
		bool bEscape = false;

		for (;;)
		{
			if ( m_cur >= m_end )
			{
				SetError( "unfinished string @ %i", Index() );
				return Token_Error;
			}

			// end
			if ( *m_cur == '\"' )
			{
				*m_cur = 0;
				token.Assign( pStart, m_cur - pStart );
				m_cur++;
				break;
			}

			if ( *m_cur != '\\' )
			{
				m_cur++;
				continue;
			}

			m_cur++;

			if ( m_cur >= m_end )
			{
				SetError( "unfinished string @ %i", Index() );
				return Token_Error;
			}

			bEscape = true;

			// Defer unescape until the end of the string is found
			switch ( *m_cur )
			{
				case '\"': case '\\': case '/':
				case 'b': case 'f':
				case 'n': case 'r': case 't':
					m_cur++;
					break;

				case 'u':
					if ( m_cur + 4 >= m_end ||
							!_isxdigit( m_cur[1] ) || !_isxdigit( m_cur[2] ) ||
							!_isxdigit( m_cur[3] ) || !_isxdigit( m_cur[4] ) )
					{
						SetError( "invalid hex escape @ %i", Index() );
						return Token_Error;
					}

					m_cur += 4;
					break;

				default:
					SetError( "invalid escape char '0x%02x' @ %i", *(unsigned char*)m_cur, Index() );
					return Token_Error;
			}
		}

		if ( bEscape )
		{
			char *cur = pStart;
			char *end = pStart + token.len;

			do
			{
				if ( cur[0] != '\\' )
				{
					cur++;
					continue;
				}

#define _shift( bytesWritten, bytesRead ) \
	memmove( cur + (bytesWritten), cur + (bytesRead), end - ( cur + (bytesRead) ) ); \
	cur += (bytesWritten); \
	end -= (bytesRead) - (bytesWritten);

				switch ( cur[1] )
				{
					case '\\':
shift_one:
						_shift( 0, 1 );
						cur++;
						break;
					case '\"': goto shift_one;
					case '/': goto shift_one;
					case 'b': cur[1] = '\b'; goto shift_one;
					case 'f': cur[1] = '\f'; goto shift_one;
					case 'n': cur[1] = '\n'; goto shift_one;
					case 'r': cur[1] = '\r'; goto shift_one;
					case 't': cur[1] = '\t'; goto shift_one;
					case 'u':
					{
						unsigned int val;
						Verify( atox( { cur + 2, 4 }, &val ) );

						if ( val <= 0xFF )
						{
							cur[0] = (char)val;

							_shift( 1, 6 );
							break;
						}
						else if ( val <= 0x7FF )
						{
							UTF8_2_FROM_UTF32( (unsigned char*)cur, val );

							_shift( 2, 6 );
							break;
						}
						else if ( UTF_SURROGATE(val) )
						{
							if ( UTF_SURROGATE_LEAD(val) )
							{
								if ( cur + 11 < end &&
										cur[6] == '\\' && cur[7] == 'u' &&
										_isxdigit( cur[8] ) && _isxdigit( cur[9] ) &&
										_isxdigit( cur[10] ) && _isxdigit( cur[11] ) )
								{
									unsigned int low;
									Verify( atox( { cur + 8, 4 }, &low ) );

									if ( UTF_SURROGATE_TRAIL( low ) )
									{
										val = UTF32_FROM_UTF16_SURROGATE( val, low );
										UTF8_4_FROM_UTF32( (unsigned char*)cur, val );

										_shift( 4, 12 );
										break;
									}
								}
							}
						}

						UTF8_3_FROM_UTF32( (unsigned char*)cur, val );

						_shift( 3, 6 );
						break;
					}
					default: UNREACHABLE();
				}

#undef _shift
			}
			while ( cur < end );

			token.len = end - pStart;
			token.ptr[token.len] = 0;
		}

		return Token_String;
	}

	char ParseNumber( string_t &token )
	{
		const char *pStart = m_cur;
		char type;

		if ( *m_cur == '-' )
		{
			m_cur++;
			if ( m_cur >= m_end )
				goto err_eof;
		}

		if ( *m_cur == '0' )
		{
			m_cur++;
			if ( m_cur >= m_end )
				goto err_eof;
		}
		else if ( IN_RANGE_CHAR( *(unsigned char*)m_cur, '1', '9' ) )
		{
			do
			{
				m_cur++;
				if ( m_cur >= m_end )
					goto err_eof;
			}
			while ( IN_RANGE_CHAR( *(unsigned char*)m_cur, '0', '9' ) );
		}
		else
		{
			SetError( "unexpected char '0x%02x' in number @ %i", *(unsigned char*)m_cur, Index() );
			return Token_Error;
		}

		type = Token_Integer;

		if ( *m_cur == '.' )
		{
			type = Token_Float;
			m_cur++;

			while ( m_cur < m_end && IN_RANGE_CHAR( *(unsigned char*)m_cur, '0', '9' ) )
				m_cur++;

			if ( m_cur >= m_end )
				goto err_eof;
		}

		if ( *m_cur == 'e' || *m_cur == 'E' )
		{
			type = Token_Float;
			m_cur++;

			if ( m_cur >= m_end )
				goto err_eof;

			if ( *m_cur == '-' || *m_cur == '+' )
			{
				m_cur++;

				if ( m_cur >= m_end )
					goto err_eof;
			}

			while ( m_cur < m_end && IN_RANGE_CHAR( *(unsigned char*)m_cur, '0', '9' ) )
				m_cur++;
		}

		token.Assign( pStart, m_cur - pStart );
		return type;

err_eof:
		SetError( "unexpected eof" );
		return Token_Error;
	}

	char ParseTable( json_table_t *pTable, string_t &token )
	{
		char type = NextToken( token );

		if ( type == '}' )
			return Token_Table;

		for (;;)
		{
			if ( type != Token_String )
			{
				SetError( "expected %s, got '0x%02x' @ %i", "string", Char(type), Index() );
				return Token_Error;
			}

			type = NextToken( token );

			if ( type != ':' )
			{
				SetError( "expected '%c', got '0x%02x' @ %i", ':', Char(type), Index() );
				return Token_Error;
			}

			json_field_t *kv = pTable->NewElement();

			Assert( token.ptr - m_start < (ostr_t::index_t)-1 );
			kv->key.ofs = token.ptr - m_start;
			kv->key.len = (ostr_t::index_t)token.len;

			type = NextToken( token );
			type = ParseValue( type, token, &kv->val );

			if ( type == Token_Error )
			{
				SetError( "invalid token '0x%02x' @ %i", Char(type), Index() );
				return Token_Error;
			}

			type = NextToken( token );

			if ( type == ',' )
			{
				type = NextToken( token );
			}
			else if ( type == '}' )
			{
				return Token_Table;
			}
			else
			{
				SetError( "expected '%c', got '0x%02x' @ %i", '}', Char(type), Index() );
				return Token_Error;
			}
		}
	}

	char ParseArray( json_array_t *pArray, string_t &token )
	{
		char type = NextToken( token );

		if ( type == ']' )
			return Token_Array;

		for (;;)
		{
			if ( type == Token_Error )
			{
				SetError( "expected '%c', got '0x%02x' @ %i", ']', Char(type), Index() );
				return Token_Error;
			}

			json_value_t *val = pArray->NewElement();
			type = ParseValue( type, token, val );

			if ( type == Token_Error )
			{
				SetError( "invalid token '0x%02x' @ %i", Char(type), Index() );
				return Token_Error;
			}

			type = NextToken( token );

			if ( type == ',' )
			{
				type = NextToken( token );
			}
			else if ( type == ']' )
			{
				return Token_Array;
			}
			else
			{
				SetError( "expected '%c', got '0x%02x' @ %i", ']', Char(type), Index() );
				return Token_Error;
			}
		}
	}

	char ParseValue( char type, string_t &token, json_value_t *value )
	{
		switch ( type )
		{
			case Token_Integer:
				if ( token.len > FMT_INT_LEN )
				{
					SetError( "invalid integer literal @ %i", Index() );
					return Token_Error;
				}

				value->type = JSON_INTEGER;
				Verify( atoi( token, &value->_integer ) );
				return type;
			case Token_Float:
				value->type = JSON_FLOAT;
				// floats are unused, ignore value
				return type;
			case Token_String:
				value->type = JSON_STRING;
				Assert( token.ptr - m_start < (ostr_t::index_t)-1 );
				value->_string.ofs = token.ptr - m_start;
				value->_string.len = (ostr_t::index_t)token.len;
				return type;
			case '{':
				value->type = JSON_TABLE;
				value->_table = (json_table_t*)m_Allocator->Alloc( sizeof(json_table_t) );
				value->_table->Init( m_start, m_Allocator );
				return ParseTable( value->_table, token );
			case '[':
				value->type = JSON_ARRAY;
				value->_array = (json_array_t*)m_Allocator->Alloc( sizeof(json_array_t) );
				value->_array->Init( m_start, m_Allocator );
				return ParseArray( value->_array, token );
			case Token_False:
			case Token_True:
				value->type = JSON_BOOL;
				value->_integer = type - Token_False;
				return type;
			case Token_Null:
				value->type = JSON_NULL;
				return type;
			default:
				return type;
		}
	}
};

#endif // SQDBG_JSON_H
