//-----------------------------------------------------------------------
//                       github.com/samisalreadytaken/sqdbg
//-----------------------------------------------------------------------
//

#ifndef SQDBG_VEC_H
#define SQDBG_VEC_H

void *sqdbg_malloc( unsigned int size );
void *sqdbg_realloc( void *p, unsigned int oldsize, unsigned int size );
void sqdbg_free( void *p, unsigned int size );

class CMemory
{
public:
#pragma pack(push, 4)
	struct memory_t
	{
		char *ptr;
		unsigned int size;
	};
#pragma pack(pop)

	memory_t memory;

	char &operator[]( unsigned int i ) const
	{
		Assert( memory.size > 0 );
		return *( memory.ptr + i );
	}

	char *Base()
	{
		return memory.ptr;
	}

	unsigned int Size() const
	{
		return memory.size;
	}

	void Free()
	{
		if ( memory.ptr )
		{
			sqdbg_free( memory.ptr, memory.size );
			memory.ptr = 0;
			memory.size = 0;
		}
	}

	void Alloc( unsigned int count )
	{
		Assert( count > 0 || memory.size == 0 );

		if ( count == memory.size )
			return;

		const int size = ( count + sizeof(void*) - 1 ) & ~( sizeof(void*) - 1 );

		if ( memory.ptr )
		{
			memory.ptr = (char*)sqdbg_realloc( memory.ptr, memory.size, size );
		}
		else
		{
			memory.ptr = (char*)sqdbg_malloc( size );
		}

		AssertOOM( memory.ptr, size );

		if ( memory.ptr )
		{
			memory.size = size;
		}
		else
		{
			memory.size = 0;
		}
	}

	void Ensure( unsigned int newcount )
	{
		unsigned int oldcount = memory.size;

		if ( newcount <= oldcount )
			return;

		if ( oldcount != 0 )
		{
			unsigned int i = (unsigned int)0x7fffffffu;

			while ( ( i >> 1 ) > newcount )
			{
				i >>= 1;
			}

			Assert( i > 0 );
			Assert( i > oldcount );
			Assert( i >= newcount );

			newcount = i;
		}
		else
		{
			if ( newcount < 4 )
				newcount = 4;
		}

		Alloc( newcount );
	}
};

template< int MEM_CACHE_CHUNKS_ALIGN = 2048 >
class CScratch
{
public:
	static const int MEM_CACHE_CHUNKSIZE = 4;
	static const int INVALID_INDEX = 0x80000000;

	struct chunk_t
	{
		char *ptr;
		int count;
	};

	chunk_t *m_Memory;
	int m_MemChunkCount;
	int m_LastFreeChunk;
	int m_LastFreeIndex;

	char *Get( int index )
	{
		Assert( index != INVALID_INDEX );

		int msgIdx = index & 0x0000ffff;
		int chunkIdx = index >> 16;

		Assert( m_Memory );
		Assert( chunkIdx < m_MemChunkCount );

		chunk_t *chunk = &m_Memory[ chunkIdx ];
		Assert( msgIdx < chunk->count );

		return &chunk->ptr[ msgIdx * MEM_CACHE_CHUNKSIZE ];
	}

	char *Alloc( int size, int *index = NULL, bool sequential = true )
	{
		if ( !m_Memory )
		{
			m_MemChunkCount = 4;
			m_Memory = (chunk_t*)sqdbg_malloc( m_MemChunkCount * sizeof(chunk_t) );
			AssertOOM( m_Memory, m_MemChunkCount * sizeof(chunk_t) );
			memset( (char*)m_Memory, 0, m_MemChunkCount * sizeof(chunk_t) );

			chunk_t *chunk = &m_Memory[0];
			chunk->count = MEM_CACHE_CHUNKS_ALIGN;
			chunk->ptr = (char*)sqdbg_malloc( chunk->count * MEM_CACHE_CHUNKSIZE );
			AssertOOM( chunk->ptr, chunk->count * MEM_CACHE_CHUNKSIZE );
			memset( chunk->ptr, 0, chunk->count * MEM_CACHE_CHUNKSIZE );
		}

		int requiredChunks;
		int msgIdx;
		int chunkIdx;
		int matchedChunks = 0;

		if ( sequential )
		{
			requiredChunks = ( size - 1 ) / MEM_CACHE_CHUNKSIZE + 1;
			msgIdx = m_LastFreeIndex;
			chunkIdx = m_LastFreeChunk;
		}
		else
		{
			requiredChunks = ( size + sizeof(int) - 1 ) / MEM_CACHE_CHUNKSIZE + 1;
			msgIdx = 0;
			chunkIdx = 0;
		}

		for (;;)
		{
			chunk_t *chunk = &m_Memory[ chunkIdx ];
			Assert( chunk->count && chunk->ptr );

			if ( sequential )
			{
				int remainingChunks = chunk->count - msgIdx;

				if ( remainingChunks >= requiredChunks )
				{
					m_LastFreeIndex = msgIdx + requiredChunks;
					m_LastFreeChunk = chunkIdx;

					if ( index )
					{
						Assert( msgIdx < 0x0000ffff );
						Assert( chunkIdx < 0x00007fff );
						*index = ( chunkIdx << 16 ) | msgIdx;
					}

					return &chunk->ptr[ msgIdx * MEM_CACHE_CHUNKSIZE ];
				}
			}
			else
			{
				char *ptr = &chunk->ptr[ msgIdx * MEM_CACHE_CHUNKSIZE ];
				Assert( *(int*)ptr >= 0 && *(int*)ptr < ( chunk->count - msgIdx ) * MEM_CACHE_CHUNKSIZE );

				if ( *(int*)ptr == 0 )
				{
					if ( ++matchedChunks == requiredChunks )
					{
						msgIdx = msgIdx - matchedChunks + 1;
						Assert( !index );
						ptr = &chunk->ptr[ msgIdx * MEM_CACHE_CHUNKSIZE ];
						*(int*)ptr = size;
						return ptr + sizeof(int);
					}
				}
				else
				{
					matchedChunks = 0;
				}

				msgIdx += ( *(int*)ptr + sizeof(int) - 1 ) / MEM_CACHE_CHUNKSIZE + 1;

				Assert( msgIdx < 0x0000ffff );

				if ( msgIdx < chunk->count )
					continue;

				matchedChunks = 0;
			}

			msgIdx = 0;

			if ( ++chunkIdx >= m_MemChunkCount )
			{
				int oldcount = m_MemChunkCount;
				m_MemChunkCount <<= 1;
				m_Memory = (chunk_t*)sqdbg_realloc( m_Memory,
						oldcount * sizeof(chunk_t),
						m_MemChunkCount * sizeof(chunk_t) );
				AssertOOM( m_Memory, m_MemChunkCount * sizeof(chunk_t) );
				memset( (char*)m_Memory + oldcount * sizeof(chunk_t),
						0,
						(m_MemChunkCount - oldcount) * sizeof(chunk_t) );
			}

			chunk = &m_Memory[ chunkIdx ];

			if ( chunk->count == 0 )
			{
				Assert( chunk->ptr == NULL );

				chunk->count = ( requiredChunks + ( MEM_CACHE_CHUNKS_ALIGN - 1 ) ) & ~( MEM_CACHE_CHUNKS_ALIGN - 1 );
				chunk->ptr = (char*)sqdbg_malloc( chunk->count * MEM_CACHE_CHUNKSIZE );
				AssertOOM( chunk->ptr, chunk->count * MEM_CACHE_CHUNKSIZE );
				memset( chunk->ptr, 0, chunk->count * MEM_CACHE_CHUNKSIZE );
			}

			Assert( chunkIdx < 0x00007fff );
		}
	}

	void Free( void *ptr )
	{
		Assert( m_Memory );
		Assert( ptr );

		*(char**)&ptr -= sizeof(int);

#ifdef _DEBUG
		bool found = false;

		for ( int chunkIdx = 0; chunkIdx < m_MemChunkCount; chunkIdx++ )
		{
			chunk_t *chunk = &m_Memory[ chunkIdx ];

			if ( chunk->count && ptr >= chunk->ptr && ptr < chunk->ptr + chunk->count * MEM_CACHE_CHUNKSIZE )
			{
				Assert( *(int*)ptr >= 0 );
				Assert( (char*)ptr + sizeof(int) + *(int*)ptr <= chunk->ptr + chunk->count * MEM_CACHE_CHUNKSIZE );
				found = true;
				break;
			}
		}

		Assert( found );

		(*(unsigned char**)&ptr)[ *(int*)ptr + sizeof(int) - 1 ] = 0xdd;
#endif

		memset( (char*)ptr, 0, *(int*)ptr + sizeof(int) );
	}

	void Free()
	{
		if ( !m_Memory )
			return;

		for ( int chunkIdx = 0; chunkIdx < m_MemChunkCount; chunkIdx++ )
		{
			chunk_t *chunk = &m_Memory[ chunkIdx ];

			if ( chunk->count )
			{
				sqdbg_free( chunk->ptr, chunk->count * MEM_CACHE_CHUNKSIZE );
			}
		}

		sqdbg_free( m_Memory, m_MemChunkCount * sizeof(chunk_t) );

		m_Memory = NULL;
		m_MemChunkCount = 4;
		m_LastFreeChunk = 0;
		m_LastFreeIndex = 0;
	}

	void ReleaseShrink()
	{
		if ( !m_Memory )
			return;

		for ( int chunkIdx = 4; chunkIdx < m_MemChunkCount; chunkIdx++ )
		{
			chunk_t *chunk = &m_Memory[ chunkIdx ];

			if ( chunk->count )
			{
				sqdbg_free( chunk->ptr, chunk->count * MEM_CACHE_CHUNKSIZE );

				chunk->count = 0;
				chunk->ptr = NULL;
			}
		}

#ifdef _DEBUG
		for ( int chunkIdx = 0; chunkIdx < 4; chunkIdx++ )
		{
			chunk_t *chunk = &m_Memory[ chunkIdx ];

			if ( chunk->count )
			{
				memset( chunk->ptr, 0xdd, chunk->count * MEM_CACHE_CHUNKSIZE );
			}
		}
#endif

		if ( m_MemChunkCount > 8 )
		{
			int oldcount = m_MemChunkCount;
			m_MemChunkCount = 8;
			m_Memory = (chunk_t*)sqdbg_realloc( m_Memory,
					oldcount * sizeof(chunk_t),
					m_MemChunkCount * sizeof(chunk_t) );
			AssertOOM( m_Memory, m_MemChunkCount * sizeof(chunk_t) );
		}

		m_LastFreeChunk = 0;
		m_LastFreeIndex = 0;
	}

	void Release()
	{
		if ( !m_Memory || ( !m_LastFreeChunk && !m_LastFreeIndex ) )
			return;

#ifdef _DEBUG
		for ( int chunkIdx = 0; chunkIdx < m_MemChunkCount; chunkIdx++ )
		{
			chunk_t *chunk = &m_Memory[ chunkIdx ];

			if ( chunk->count )
			{
				memset( chunk->ptr, 0xdd, chunk->count * MEM_CACHE_CHUNKSIZE );
			}
		}
#endif

		m_LastFreeChunk = 0;
		m_LastFreeIndex = 0;
	}
};

template < typename T, bool bExternalMem = false, class CAllocator = CMemory >
class vector
{
public:
	typedef unsigned int I;

	CAllocator _base;
	I _size;

	vector() : _base(), _size(0)
	{
		Assert( !bExternalMem );
	}

	vector( CAllocator &a ) : _base(a), _size(0)
	{
		Assert( bExternalMem );
	}

	vector( I count ) : _base(), _size(0)
	{
		Assert( !bExternalMem );
		_base.Alloc( count * sizeof(T) );
	}

	vector( const vector< T > &src ) : _base()
	{
		Assert( !bExternalMem );
		_base.Alloc( src._base.Size() );
		_size = src._size;

		for ( I i = 0; i < _size; i++ )
			new( &_base[ i * sizeof(T) ] ) T( (T&)src._base[ i * sizeof(T) ] );
	}

	~vector()
	{
		Assert( (unsigned int)_size <= _base.Size() );

		for ( I i = 0; i < _size; i++ )
			((T&)(_base[ i * sizeof(T) ])).~T();

		if ( !bExternalMem )
			_base.Free();
	}

	T &operator[]( I i ) const
	{
		Assert( _size > 0 );
		Assert( i >= 0 && i < _size );
		Assert( _size * sizeof(T) <= _base.Size() );
		return (T&)_base[ i * sizeof(T) ];
	}

	T *base()
	{
		return _base.Base();
	}

	I size() const
	{
		return _size;
	}

	I capacity() const
	{
		return _base.Size() / sizeof(T);
	}

	T &top() const
	{
		Assert( _size > 0 );
		return (T&)_base[ ( _size - 1 ) * sizeof(T) ];
	}

	void pop()
	{
		Assert( _size > 0 );
		((T&)_base[ --_size * sizeof(T) ]).~T();
	}

	T &append()
	{
		_base.Ensure( ++_size * sizeof(T) );
		Assert( _size * sizeof(T) <= _base.Size() );
		return *( new( &_base[ ( _size - 1 ) * sizeof(T) ] ) T() );
	}

	void append( const T &src )
	{
		_base.Ensure( ++_size * sizeof(T) );
		Assert( _size * sizeof(T) <= _base.Size() );
		new( &_base[ ( _size - 1 ) * sizeof(T) ] ) T( src );
	}

	T &insert( I i )
	{
		Assert( i >= 0 && i <= _size );

		_base.Ensure( ++_size * sizeof(T) );
		Assert( _size * sizeof(T) <= _base.Size() );

		if ( i != _size - 1 )
		{
			memmove( &_base[ ( i + 1 ) * sizeof(T) ],
					&_base[ i * sizeof(T) ],
					( _size - ( i + 1 ) ) * sizeof(T) );
		}

		return *( new( &_base[ i * sizeof(T) ] ) T() );
	}

	void remove( I i )
	{
		Assert( _size > 0 );
		Assert( i >= 0 && i < _size );

		((T&)_base[ i * sizeof(T) ]).~T();

		if ( i != _size - 1 )
		{
			memmove( &_base[ i * sizeof(T) ],
					&_base[ ( i + 1 ) * sizeof(T) ],
					( _size - ( i + 1 ) ) * sizeof(T) );
		}

		_size--;
	}

	void clear()
	{
		for ( I i = 0; i < _size; i++ )
			((T&)_base[ i * sizeof(T) ]).~T();

		_size = 0;
	}

	void sort( int (*fn)(const T *, const T *) )
	{
		Assert( _size * sizeof(T) <= _base.Size() );

		if ( _size > 1 )
		{
			qsort( _base.Base(), _size, sizeof(T), (int (*)(const void *, const void *))fn );
		}
	}

	void reserve( I count )
	{
		Assert( (unsigned int)_size <= _base.Size() );

		if ( count == 0 )
			count = 4;

		if ( (unsigned int)count == _base.Size() )
			return;

		for ( I i = count; i < _size; i++ )
			((T&)_base[ i * sizeof(T) ]).~T();

		_base.Alloc( count * sizeof(T) );
	}

	void purge()
	{
		Assert( _size * sizeof(T) <= _base.Size() );

		for ( I i = 0; i < _size; i++ )
			((T&)_base[ i * sizeof(T) ]).~T();

		_base.Free();
		_size = 0;
	}
};

class CBuffer
{
public:
	CMemory _base;
	int _size;
	int _offset;

	char *base()
	{
		return _base.Base() + _offset;
	}

	int size() const
	{
		return _size;
	}

	int capacity() const
	{
		return _base.Size();
	}

	void reserve( int count )
	{
		Assert( (unsigned int)_size <= _base.Size() );

		if ( (unsigned int)count == _base.Size() )
			return;

		_base.Alloc( count );
	}

	void purge()
	{
		_base.Free();
		_size = 0;
		_offset = 0;
	}
};

class CBufTmpCache
{
public:
	CBuffer *buffer;
	int size;

	CBufTmpCache( CBuffer *b ) :
		buffer(b),
		size(buffer->_size)
	{
		buffer->_offset += buffer->_size;
		buffer->_size = 0;
	}

	~CBufTmpCache()
	{
		buffer->_offset -= size;
		buffer->_size = size;
	}
};

#endif // SQDBG_VEC_H
