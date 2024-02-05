//-----------------------------------------------------------------------
//                       github.com/samisalreadytaken/sqdbg
//-----------------------------------------------------------------------
//

#ifndef SQDBG_JSON_H
#define SQDBG_JSON_H

#include <new>
#include <squirrel.h>
#include <squtils.h>
#include "str.h"

typedef enum
{
	JSON_NULL		= 0x00000000,
	JSON_INTEGER	= 0x00000001,
	JSON_FLOAT		= 0x00000002,
	JSON_STRING		= 0x00000004,
	JSON_TABLE		= 0x00000008,
	JSON_ARRAY		= 0x00000010,
	JSON_BOOL		= 0x00000020,
	_ALLOCATED		= 0x01000000
} JSONTYPE;

class json_table_t;
class json_array_t;
struct json_value_t;

static inline json_table_t *CreateTable( int reserve );
static inline json_array_t *CreateArray( int reserve );
static inline void DeleteTable( json_table_t *p );
static inline void DeleteArray( json_array_t *p );
static inline void FreeValue( json_value_t &val );

struct json_value_t
{
	json_value_t() {}

	union
	{
		string_t _s;
		int _n;
		SQFloat _f;
		json_table_t *_t;
		json_array_t *_a;
	};

	int type;
};

struct json_field_t
{
	json_field_t() {}

	string_t key;
	json_value_t val;
};

class json_array_t
{
private:
	typedef sqvector< json_value_t > array_t;
	array_t value;

	void clear()
	{
		for ( int i = value.size(); i--; )
		{
			json_value_t &elem = value._vals[i];

			if ( elem.type & _ALLOCATED )
			{
				FreeValue( elem );
			}

			value.pop_back();
		}
	}

public:
	json_array_t() {}

	json_array_t( int reserve )
	{
		if ( reserve )
			value.reserve( reserve );
	}

	~json_array_t()
	{
		clear();
	}

	json_value_t &operator[]( int i ) const { return value._vals[i]; }
	unsigned int size() const { return value.size(); }
	void remove( int i ) { value.remove(i); }

	json_value_t *Get( int i )
	{
		Assert( i >= 0 && i < (int)value.size() );
		return &value._vals[i];
	}

	json_value_t *NewElement()
	{
		json_value_t &ret = value.push_back( json_value_t{} );
		memset( &ret, 0, sizeof(ret) );
		return &ret;
	}

	json_table_t &AppendTable( int reserve = 0 )
	{
		json_value_t &elem = value.push_back( json_value_t{} );

		elem.type = JSON_TABLE | _ALLOCATED;
		elem._t = CreateTable( reserve );

		return *elem._t;
	}

	json_array_t &AppendArray( int reserve = 0 )
	{
		json_value_t &elem = value.push_back( json_value_t{} );

		elem.type = JSON_ARRAY | _ALLOCATED;
		elem._a = CreateArray( reserve );

		return *elem._a;
	}

	void Append( int val )
	{
		json_value_t &elem = value.push_back( json_value_t{} );
		elem.type = JSON_INTEGER;
		elem._n = val;
	}

	void Append( float val )
	{
		json_value_t &elem = value.push_back( json_value_t{} );
		elem.type = JSON_FLOAT;
		elem._f = val;
	}

	void Append( string_t val )
	{
		json_value_t &elem = value.push_back( json_value_t{} );
		elem.type = JSON_STRING;
		elem._s.Assign( val );
	}
};

class json_table_t
{
private:
	typedef sqvector< json_field_t > table_t;
	table_t value;

	void clear()
	{
		for ( int i = value.size(); i--; )
		{
			json_value_t &val = value._vals[i].val;

			if ( val.type & _ALLOCATED )
			{
				FreeValue( val );
			}

			value.pop_back();
		}
	}

public:
	json_table_t() {}

	json_table_t( int reserve )
	{
		if ( reserve )
			value.reserve( reserve );
	}

	~json_table_t()
	{
		clear();
	}

	unsigned int size() const { return value.size(); }

	json_field_t *Get( int i )
	{
		Assert( i >= 0 && i < (int)value.size() );
		return &value._vals[i];
	}

	json_value_t *Get( const string_t &key )
	{
		for ( unsigned int i = 0; i < value.size(); i++ )
		{
			json_field_t *kv = &value._vals[i];
			if ( kv->key.IsEqualTo( key ) )
				return &kv->val;
		}

		return NULL;
	}

	json_value_t *Get( const string_t &key ) const
	{
		return const_cast< json_table_t * >( this )->Get( key );
	}

	json_value_t *GetOrCreate( const string_t &key )
	{
		json_value_t *val = Get( key );

		if ( !val )
		{
			json_field_t *kv = NewElement();
			kv->key.Assign( key );
			val = &kv->val;
		}
		else if ( val->type & _ALLOCATED )
		{
			FreeValue( *val );
		}

		return val;
	}

	json_field_t *NewElement()
	{
		json_field_t &ret = value.push_back( json_field_t{} );
		memset( &ret, 0, sizeof(ret) );
		return &ret;
	}

	bool GetBool( const string_t &key, bool *out ) const
	{
		json_value_t *kval = Get( key );

		if ( kval && ( kval->type & JSON_BOOL ) )
		{
			*out = kval->_n;
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
			*out = kval->_n;
			return true;
		}

		*out = defaultVal;
		return false;
	}

	bool GetFloat( const string_t &key, float *out, float defaultVal = 0.0f ) const
	{
		json_value_t *kval = Get( key );

		if ( kval && ( kval->type & JSON_FLOAT ) )
		{
			*out = kval->_f;
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
			out->Assign( kval->_s );
			return true;
		}

		out->Assign( defaultVal );
		return false;
	}

	bool GetTable( const string_t &key, json_table_t **out ) const
	{
		json_value_t *kval = Get( key );

		if ( kval && ( kval->type & JSON_TABLE ) )
		{
			*out = kval->_t;
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
			*out = kval->_a;
			return true;
		}

		*out = NULL;
		return false;
	}

	void SetNull( const string_t &key )
	{
		json_value_t *kval = GetOrCreate( key );

		kval->type = JSON_NULL;
	}

	void SetBool( const string_t &key, bool val )
	{
		json_value_t *kval = GetOrCreate( key );

		kval->type = JSON_BOOL;
		kval->_n = val;
	}

	template < typename T >
	void SetInt( const string_t &key, T *val )
	{
		SetInt( key, (int)val );
	}

	void SetInt( const string_t &key, int val )
	{
		json_value_t *kval = GetOrCreate( key );

		kval->type = JSON_INTEGER;
		kval->_n = val;
	}

	void SetFloat( const string_t &key, float val )
	{
		json_value_t *kval = GetOrCreate( key );

		kval->type = JSON_FLOAT;
		kval->_f = val;
	}

	template < int size >
	void SetString( const string_t &key, const char (&val)[size] )
	{
		json_value_t *kval = GetOrCreate( key );

		kval->type = JSON_STRING;
		kval->_s.Assign( val, size - 1 );
	}

	void SetString( const string_t &key, const string_t &val )
	{
		if ( !val.ptr )
			return;

		json_value_t *kval = GetOrCreate( key );

		kval->type = JSON_STRING | _ALLOCATED;
		kval->_s.Copy( val );
	}

	void SetStringNoCopy( const string_t &key, const string_t &val )
	{
		if ( !val.ptr )
			return;

		json_value_t *kval = GetOrCreate( key );

		kval->type = JSON_STRING;
		kval->_s.Assign( val );
	}

#ifdef SQUNICODE
	void SetStringNoCopy( const string_t &key, const sqstring_t &val )
	{
		SetString( key, val );
	}

	void SetString( const string_t &key, sqstring_t val )
	{
		if ( !val.ptr )
			return;

		json_value_t *kval = GetOrCreate( key );

		char tmp[4096];
		int len = UnicodeToUTF8( tmp, val.ptr, sizeof(tmp) );

		kval->type = JSON_STRING | _ALLOCATED;
		kval->_s.Copy( tmp, len );
	}
#endif

	json_table_t &SetTable( const string_t &key, int reserve = 0 )
	{
		json_value_t *kval = GetOrCreate( key );

		kval->type = JSON_TABLE | _ALLOCATED;
		kval->_t = CreateTable( reserve );

		return *kval->_t;
	}

	json_array_t &SetArray( const string_t &key, int reserve = 0 )
	{
		json_value_t *kval = GetOrCreate( key );

		kval->type = JSON_ARRAY | _ALLOCATED;
		kval->_a = CreateArray( reserve );

		return *kval->_a;
	}

	void SetArray( const string_t &key, json_array_t &val )
	{
		json_value_t *kval = GetOrCreate( key );

		kval->type = JSON_ARRAY;
		kval->_a = &val;
	}

	void Set( const string_t &key, bool val ) { SetBool( key, val ); }
	void Set( const string_t &key, int val ) { SetInt( key, val ); }
	void Set( const string_t &key, unsigned int val ) { SetInt( key, val ); }
	void Set( const string_t &key, float val ) { SetFloat( key, val ); }
	void Set( const string_t &key, const string_t &val ) { SetString( key, val ); }

	bool Get( const string_t &key, bool *out ) const { return GetBool( key, out ); }
	bool Get( const string_t &key, int *out, int defaultVal = 0 ) const { return GetInt( key, out, defaultVal ); }
	bool Get( const string_t &key, float *out, float defaultVal = 0.0f ) const { return GetFloat( key, out, defaultVal ); }
	bool Get( const string_t &key, string_t *out, const char *defaultVal = "" ) const { return GetString( key, out, defaultVal ); }
	bool Get( const string_t &key, json_table_t **out ) const { return GetTable( key, out ); }
	bool Get( const string_t &key, json_array_t **out ) const { return GetArray( key, out ); }

private:
	template < typename T > void Set( const string_t &, T* );
	template < typename T > void Set( const string_t &, T );
	template < typename T > void SetBool( const string_t &, T );
};

inline json_table_t *CreateTable( int reserve )
{
	json_table_t *ret = (json_table_t *)sqdbg_malloc( sizeof(json_table_t) );
	new (ret) json_table_t( reserve );
	return ret;
}

inline json_array_t *CreateArray( int reserve )
{
	json_array_t *ret = (json_array_t *)sqdbg_malloc( sizeof(json_array_t) );
	new (ret) json_array_t( reserve );
	return ret;
}

inline void DeleteTable( json_table_t *p )
{
	p->~json_table_t();
	sqdbg_free( p, sizeof(json_table_t) );
}

inline void DeleteArray( json_array_t *p )
{
	p->~json_array_t();
	sqdbg_free( p, sizeof(json_array_t) );
}

inline void FreeValue( json_value_t &val )
{
	Assert( val.type & _ALLOCATED );

	switch ( val.type & (~_ALLOCATED) )
	{
		case JSON_ARRAY:
		{
			DeleteArray( val._a );
			break;
		}
		case JSON_TABLE:
		{
			DeleteTable( val._t );
			break;
		}
		case JSON_STRING:
		{
			val._s.Free();
			break;
		}
		default: UNREACHABLE();
	}
}

inline int GetJSONStringSize( const json_value_t &obj )
{
	switch ( obj.type & (~_ALLOCATED) )
	{
		case JSON_STRING:
		{
			int len = 2 + obj._s.len; // "val"
			const char *c = obj._s.ptr;
			for ( int i = obj._s.len; i--; c++ )
			{
				switch ( *c )
				{
					case '\"': case '\\': case '\b':
					case '\f': case '\n': case '\r': case '\t':
						len++;
				}
			}
			return len;
		}
		case JSON_INTEGER:
		{
			if ( obj._n > 0 )
				return countdigits( obj._n );

			if ( obj._n )
				return countdigits( -obj._n ) + 1;

			return 1; // 0
		}
		case JSON_FLOAT:
		{
			char tmp[ FMT_FLT_LEN + 1 ];
			return snprintf( tmp, sizeof(tmp), "%f", obj._f );
		}
		case JSON_BOOL:
		{
			return obj._n ? STRLEN("true") : STRLEN("false");
		}
		case JSON_NULL:
		{
			return STRLEN("null");
		}
		case JSON_TABLE:
		{
			int size = 2; // {}
			if ( obj._t->size() )
			{
				for ( unsigned int i = 0; i < obj._t->size(); i++ )
				{
					json_field_t *kv = obj._t->Get(i);
					size += 2 + kv->key.len + 1 + GetJSONStringSize( kv->val ) + 1; // "key":val,
				}
				size--; // trailing comma
			}
			return size;
		}
		case JSON_ARRAY:
		{
			int size = 2; // []
			if ( obj._a->size() )
			{
				for ( unsigned int i = 0; i < obj._a->size(); i++ )
					size += GetJSONStringSize( *obj._a->Get(i) ) + 1;
				size--; // trailing comma
			}
			return size;
		}
		default: return 0;
	}
}

inline int JSONStringify( const json_value_t &obj, char *mem, int size, int idx )
{
	switch ( obj.type & (~_ALLOCATED) )
	{
		case JSON_STRING:
		{
			mem[idx++] = '\"';

			const char *c = obj._s.ptr;
			for ( int i = obj._s.len; i--; c++ )
			{
				Assert( *c );
				mem[idx++] = *c;

				switch ( *c )
				{
					case '\"':
						mem[idx-1] = '\\';
						mem[idx++] = '\"';
						break;
					case '\\':
						mem[idx++] = '\\';
						break;
					case '\b':
						mem[idx-1] = '\\';
						mem[idx++] = 'b';
						break;
					case '\f':
						mem[idx-1] = '\\';
						mem[idx++] = 'f';
						break;
					case '\n':
						mem[idx-1] = '\\';
						mem[idx++] = 'n';
						break;
					case '\r':
						mem[idx-1] = '\\';
						mem[idx++] = 'r';
						break;
					case '\t':
						mem[idx-1] = '\\';
						mem[idx++] = 't';
						break;
				}
			}

			mem[idx++] = '\"';
			break;
		}
		case JSON_INTEGER:
		{
			idx += printint( mem + idx, size, obj._n );
			break;
		}
		case JSON_FLOAT:
		{
			idx += sprintf( mem + idx, "%f", obj._f );
			break;
		}
		case JSON_BOOL:
		{
			if ( obj._n )
			{
				memcpy( mem + idx, "true", STRLEN("true") );
				idx += STRLEN("true");
			}
			else
			{
				memcpy( mem + idx, "false", STRLEN("false") );
				idx += STRLEN("false");
			}
			break;
		}
		case JSON_NULL:
		{
			memcpy( mem + idx, "null", STRLEN("null") );
			idx += STRLEN("null");
			break;
		}
		case JSON_TABLE:
		{
			mem[idx++] = '{';
			if ( obj._t->size() )
			{
				for ( unsigned int i = 0; i < obj._t->size(); i++ )
				{
					json_field_t *kv = obj._t->Get(i);
					mem[idx++] = '\"';
					memcpy( mem + idx, kv->key.ptr, kv->key.len );
					idx += kv->key.len;
					mem[idx++] = '\"';
					mem[idx++] = ':';
					idx = JSONStringify( kv->val, mem, size, idx );
					mem[idx++] = ',';
				}
				idx--; // trailing comma
			}
			mem[idx++] = '}';
			break;
		}
		case JSON_ARRAY:
		{
			mem[idx++] = '[';
			if ( obj._a->size() )
			{
				for ( unsigned int i = 0; i < obj._a->size(); i++ )
				{
					idx = JSONStringify( *obj._a->Get(i), mem, size, idx );
					mem[idx++] = ',';
				}
				idx--; // trailing comma
			}
			mem[idx++] = ']';
			break;
		}
		default: Assert(0);
	}
	Assert( idx < size );
	return idx;
}

inline int GetJSONStringSize( json_table_t *obj )
{
	json_value_t v;
	v.type = JSON_TABLE;
	v._t = obj;
	return GetJSONStringSize( v );
}

inline int JSONStringify( json_table_t *obj, char *mem, int size, int idx )
{
	json_value_t v;
	v.type = JSON_TABLE;
	v._t = obj;
	return JSONStringify( v, mem, size, idx );
}

class JSONParser
{
private:
	char *m_cur;
	char *m_end;
	char m_error[64];

	enum Token
	{
		Error = -1,
		String = 1,
		Integer,
		Float,
		True,
		False,
		Null,
		Table = '{',
		Array = '[',
	};

public:
	JSONParser( char *ptr, int len, json_table_t *pTable )
	{
		m_error[0] = 0;

		m_cur = ptr;
		m_end = ptr + len + 1;

		string_t token;
		int type;
		NextToken( type, token );

		if ( type == '{' )
		{
			ParseTable( pTable, type, token );
		}
		else
		{
			SetError( "missing '{'" );
		}
	}

	const char *GetError() const
	{
		return m_error[0] ? m_error : NULL;
	}

private:
	void SetError( const char *fmt, ... )
	{
		if ( m_error[0] )
			return;

		va_list va;
		va_start( va, fmt );
		int len = vsnprintf( m_error, sizeof(m_error), fmt, va );
		va_end( va );

		if ( len >= sizeof(m_error) )
			len = sizeof(m_error)-1;

		if ( sizeof(m_error) - len > 4 )
		{
			// negative offset
			m_error[len++] = '@';
			m_error[len++] = '-';
			len += printint( m_error + len, sizeof(m_error) - len, m_end - m_cur - 1 );
			m_error[len] = 0;
		}
	}

	void NextToken( int &type, string_t &token )
	{
		type = Token::Error;

		while ( m_cur < m_end )
		{
			switch ( *m_cur )
			{
				// ws
				case 0x20: case 0x0A: case 0x0D: case 0x09:
					m_cur++;
					break;

				case '\"':
					ParseString( type, token );
					return;

				case '-':
				case '0': case '1': case '2': case '3': case '4':
				case '5': case '6': case '7': case '8': case '9':
					ParseNumber( type, token );
					return;

				case ':': case ',':
				case '{': case '}':
				case '[': case ']':
					type = *m_cur++;
					return;

				case 't':
					if ( m_cur + 4 >= m_end ||
							m_cur[1] != 'r' || m_cur[2] != 'u' || m_cur[3] != 'e' )
					{
						SetError( "invalid token, expected 'true'" );
						return;
					}

					type = Token::True;
					m_cur += 4;
					return;

				case 'f':
					if ( m_cur + 5 >= m_end ||
							m_cur[1] != 'a' || m_cur[2] != 'l' || m_cur[3] != 's' || m_cur[4] != 'e' )
					{
						SetError( "invalid token, expected 'false'" );
						return;
					}

					type = Token::False;
					m_cur += 5;
					return;

				case 'n':
					if ( m_cur + 4 >= m_end ||
							m_cur[1] != 'u' || m_cur[2] != 'l' || m_cur[3] != 'l' )
					{
						SetError( "invalid token, expected 'null'" );
						return;
					}

					type = Token::Null;
					m_cur += 4;
					return;

				default:
					SetError( "invalid token '0x%02x'", *m_cur );
					return;
			}
		}
	}

	void ParseString( int &type, string_t &token )
	{
		char *pStart = m_cur + 1;
		bool bEscape = false;

		while ( m_cur++ < m_end )
		{
			// end
			if ( *m_cur == '\"' )
			{
				*m_cur = 0;
				int len = m_cur - pStart;
				token.Assign( pStart, len );
				type = Token::String;
				m_cur++;
				break;
			}

			// not escape char
			if ( *m_cur != '\\' )
				continue;

			if ( m_cur++ >= m_end )
			{
				SetError( "unclosed string" );
				return;
			}

			bEscape = true;

			switch ( *m_cur )
			{
				case '\"': case '\\': case '/': case 'b':
				case 'f': case 'n': case 'r': case 't':
					break;

				case 'u':
					if ( m_cur + 4 >= m_end ||
							!isxdigit( m_cur[1] ) || !isxdigit( m_cur[2] ) ||
							!isxdigit( m_cur[3] ) || !isxdigit( m_cur[4] ) )
					{
						SetError( "invalid \\u number" );
						return;
					}
					m_cur += 3;
					break;

				default:
					SetError( "invalid escape char '0x%02X'", *m_cur );
					return;
			}
		}

		if ( bEscape )
		{
			Assert( type == Token::String );

			int len = token.len;
			char *c = pStart;
			for ( int i = 0; i < len; i++, c++ )
			{
				if ( c[0] != '\\' )
					continue;

				switch ( c[1] )
				{
					case '\"': *c = '\"'; goto shift_one;
					case '\\': *c = '\\'; goto shift_one;
					case '/': *c = '/'; goto shift_one;
					case 'b': *c = '\b'; goto shift_one;
					case 'f': *c = '\f'; goto shift_one;
					case 'n': *c = '\n'; goto shift_one;
					case 'r': *c = '\r'; goto shift_one;
					case 't': *c = '\t'; goto shift_one;
					case 'u':
					{
						int val = 0;
						Verify( atox( { c + 2, 4 }, &val ) );

						if ( val & 0xFF00 )
						{
							c[0] = ( val >> 8 ) & 0xFF;
							c[1] = val & 0xFF;

							len -= 4;
							memmove( c + 2, c + 6, len - i );
						}
						else
						{
							c[0] = (char)val;

							len -= 5;
							memmove( c + 1, c + 6, len - i );
						}

						break;
					}
					default:
						SetError( "invalid escape char '0x%02x'", c[1] );
						return;
				}

				continue;

			shift_one:
				memmove( c + 1, c + 2, len - i );
				len--;
			}

			token.len = len;
		}
	}

	void ParseNumber( int &type, string_t &token )
	{
		const char *pStart = m_cur;

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
		else if ( *m_cur >= '1' && *m_cur <= '9' )
		{
			do
			{
				m_cur++;
				if ( m_cur >= m_end )
					goto err_eof;
			}
			while ( *m_cur >= '0' && *m_cur <= '9' );
		}
		else
		{
			SetError( "unexpected char '0x%02x' in number", *m_cur );
			return;
		}

		type = Token::Integer;

		if ( *m_cur == '.' )
		{
			type = Token::Float;
			m_cur++;

			while ( m_cur < m_end && ( *m_cur >= '0' && *m_cur <= '9' ) )
				m_cur++;

			if ( m_cur >= m_end )
				goto err_eof;
		}

		if ( *m_cur == 'e' || *m_cur == 'E' )
		{
			type = Token::Float;
			m_cur++;

			if ( *m_cur == '-' || *m_cur == '+' )
				m_cur++;

			while ( m_cur < m_end && ( *m_cur >= '0' && *m_cur <= '9' ) )
				m_cur++;

			if ( m_cur >= m_end )
				goto err_eof;
		}

		token.Assign( pStart, m_cur - pStart );
		return;

	err_eof:
		SetError( "unexpected eof in number" );
		return;
	}

	void ParseTable( json_table_t *pTable, int &type, string_t &token )
	{
		Assert( type == '{' );
		if ( *m_cur == '}' )
		{
			m_cur++;
			return;
		}
		do
		{
			NextToken( type, token );

			if ( type != Token::String )
			{
				SetError( "invalid token, expected string" );
				return;
			}

			NextToken( type, token );

			if ( type != ':' )
			{
				SetError( "invalid token, expected ':'" );
				return;
			}

			Assert( !pTable->Get( token ) );

			json_field_t *kv = pTable->NewElement();
			kv->key.Assign( token );

			NextToken( type, token );

			ParseValue( type, token, &kv->val );

			if ( GetError() )
				return;

			NextToken( type, token );

			if ( type != ',' && type != '}' )
			{
				SetError( "invalid token, expected '}'" );
				return;
			}
		} while ( type != '}' );
	}

	void ParseArray( json_array_t *pArray, int &type, string_t &token )
	{
		Assert( type == '[' );
		if ( *m_cur == ']' )
		{
			m_cur++;
			return;
		}
		do
		{
			NextToken( type, token );

			if ( GetError() )
				return;

			json_value_t *val = pArray->NewElement();
			ParseValue( type, token, val );

			if ( GetError() )
				return;

			NextToken( type, token );

			if ( type != ',' && type != ']' )
			{
				SetError( "invalid token, expected ']'" );
				return;
			}
		} while ( type != ']' );
	}

	void ParseValue( int type, string_t &token, json_value_t *value )
	{
		value->type = JSON_NULL;

		switch ( type )
		{
			case Token::Integer:
			{
				value->type = JSON_INTEGER;
				Verify( atoi( token, &value->_n ) );
				return;
			}
			case Token::Float:
			{
				char *pEnd = token.ptr + token.len;
				char cEnd = *pEnd;
				*pEnd = 0;
				value->type = JSON_FLOAT;
				value->_f = (SQFloat)atof( token.ptr );
				*pEnd = cEnd;
				return;
			}
			case Token::String:
				value->type = JSON_STRING;
				value->_s.Assign( token );
				return;
			case '{':
				value->type = JSON_TABLE | _ALLOCATED;
				value->_t = CreateTable(0);
				ParseTable( value->_t, type, token );
				return;
			case '[':
				value->type = JSON_ARRAY | _ALLOCATED;
				value->_a = CreateArray(0);
				ParseArray( value->_a, type, token );
				return;
			case Token::True:
				value->type = JSON_BOOL;
				value->_n = 1;
				return;
			case Token::False:
				value->type = JSON_BOOL;
				value->_n = 0;
				return;
			case Token::Null:
				return;
			default:
				SetError( "unrecognised token" );
				return;
		}
	}
};

#endif // SQDBG_JSON_H
