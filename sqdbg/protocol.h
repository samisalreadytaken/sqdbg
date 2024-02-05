//-----------------------------------------------------------------------
//                       github.com/samisalreadytaken/sqdbg
//-----------------------------------------------------------------------
//

#ifndef SQDBG_DAP_H
#define SQDBG_DAP_H

class json_table_t;
struct json_value_t;
int GetJSONStringSize( json_table_t *obj );
int JSONStringify( json_table_t *obj, char *mem, int size, int idx );
template < typename I > int countdigits( I input );
void *sqdbg_malloc( unsigned int size );
void sqdbg_free( void *p, unsigned int size );

#define DAP_HEADER_START "Content-Length: "
#define DAP_HEADER_END "\r\n\r\n"

#define STRLEN(s) (sizeof(s) - 1)

#define STRCMP( s, StrLiteral )\
	memcmp( (s), (StrLiteral), sizeof(StrLiteral)-1 )

inline void DAP_Serialise( json_table_t *table, char **jsonptr, int *jsonlen )
{
	int contentSize = GetJSONStringSize( table );
	int idx = countdigits( contentSize );
	int size = STRLEN( DAP_HEADER_START DAP_HEADER_END ) + idx + contentSize + 1;

	char *mem = (char*)sqdbg_malloc( ( size + 3 ) & ~3 );

	memcpy( mem, DAP_HEADER_START, STRLEN( DAP_HEADER_START ) );
	idx += STRLEN( DAP_HEADER_START );

	for ( int i = idx - 1; contentSize; )
	{
		char c = contentSize % 10;
		contentSize /= 10;
		mem[i--] = '0' + c;
	}

	memcpy( mem + idx, DAP_HEADER_END, STRLEN( DAP_HEADER_END ) );
	idx += STRLEN( DAP_HEADER_END );

	idx = JSONStringify( table, mem, size, idx );
	mem[idx] = 0;

	Assert( idx == size-1 );

	*jsonptr = mem;
	*jsonlen = idx;
}

inline void DAP_Free( char *jsonptr, int jsonlen )
{
	sqdbg_free( jsonptr, ( ( jsonlen + 1 ) + 3 ) & ~3 );
}

inline bool DAP_ReadHeader( char **ppMsg, int *pLength )
{
	char *pMsg = *ppMsg;

	if ( STRCMP( pMsg, DAP_HEADER_START ) != 0 )
		return false;

	pMsg += STRLEN( DAP_HEADER_START );

	char *pEnd = (char*)strstr( pMsg, DAP_HEADER_END );
	if ( !pEnd )
		return false;

	int nContentLength = 0;

	for ( char *c = pMsg; c < pEnd; c++ )
	{
		if ( *c >= '0' && *c <= '9' )
		{
			nContentLength = nContentLength * 10 + *c - '0';
		}
		else return false;
	}

	if ( !nContentLength )
		return false;

	*pLength = nContentLength;
	*ppMsg = pEnd + STRLEN( DAP_HEADER_END );

	return true;
}

#undef DAP_HEADER_START
#undef DAP_HEADER_END

#undef STRLEN
#undef STRCMP


#define DAP_START_REQUEST( _seq, _cmd ) \
	if ( IsClientConnected() ) \
	{ \
		json_table_t packet(4); \
		packet.SetInt( "seq", _seq ); \
		packet.SetString( "type", "request" ); \
		packet.SetString( "command", _cmd );

#define _DAP_START_RESPONSE( _seq, _cmd, _suc, _elemcount ) \
	if ( IsClientConnected() ) \
	{ \
		json_table_t packet(4 + _elemcount); \
		packet.SetInt( "request_seq", _seq ); \
		packet.SetString( "type", "response" ); \
		packet.SetString( "command", _cmd ); \
		packet.SetBool( "success", _suc );

#define DAP_START_RESPONSE( _seq, _cmd ) \
		_DAP_START_RESPONSE( _seq, _cmd, true, 1 );

#define DAP_ERROR_RESPONSE( _seq, _cmd ) \
		_DAP_START_RESPONSE( _seq, _cmd, false, 1 );

#define DAP_ERROR_BODY( _id, _fmt, _elemcount ) \
		DAP_SET_TABLE( body, 1 ); \
		json_table_t &error = body.SetTable( "error", 2 + _elemcount ); \
		error.SetInt( "id", _id ); \
		error.SetString( "format", _fmt ); \

#define DAP_START_EVENT( _seq, _ev ) \
	if ( IsClientConnected() ) \
	{ \
		json_table_t packet(4); \
		packet.SetInt( "seq", _seq ); \
		packet.SetString( "type", "event" ); \
		packet.SetString( "event", _ev );

#define DAP_SET( _key, _val ) \
		packet.Set( _key, _val );

#define DAP_SET_TABLE( _val, _elemcount ) \
		json_table_t &_val = packet.SetTable( #_val, _elemcount )

#define DAP_SEND() \
		{ \
			char *jsonptr; \
			int jsonlen; \
			DAP_Serialise( &packet, &jsonptr, &jsonlen ); \
			Send( jsonptr, jsonlen ); \
			DAP_Free( jsonptr, jsonlen ); \
		} \
	}

#endif // SQDBG_DAP_H
