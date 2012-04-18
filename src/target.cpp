 /*
 * \file target.cpp
 */

#include "logog.hpp"

#include <iostream>

#ifdef LOGOG_FLAVOR_POSIX
#include <unistd.h>
#endif

namespace logog {

	namespace color {
	enum LogogColor 
{
		COLOR_DEFAULT,
		COLOR_RED,
		COLOR_GREEN,
		COLOR_YELLOW
	};

#ifdef LOGOG_FLAVOR_WINDOWS
	// Returns the character attribute for the given color.
	WORD GetColorAttribute(LogogColor color) 
	{
		switch (color) {
			case COLOR_RED:    return FOREGROUND_RED;
			case COLOR_GREEN:  return FOREGROUND_GREEN;
			case COLOR_YELLOW: return FOREGROUND_RED | FOREGROUND_GREEN;
			default:           return 0;
		}
	}
#else
	// Returns the ANSI color code for the given color.  COLOR_DEFAULT is
	// an invalid input.
	const char* GetAnsiColorCode(LogogColor color) 
	{
		switch (color) {
			case COLOR_RED:     return "1";
			case COLOR_GREEN:   return "2";
			case COLOR_YELLOW:  return "3";
			default:            return NULL;
		};
	}
#endif  // LOGOG_FLAVOR_WINDOWS

	bool ShouldUseColor(bool stdout_is_tty) 
	{
#ifdef LOGOG_FLAVOR_WINDOWS
		// On Windows the TERM variable is usually not set, but the
		// console there does support colors.
		return stdout_is_tty;
#else
		// On non-Windows platforms, we rely on the TERM variable.
#if defined(TODO_COLOR_SUPPORT)
		const char* const term = posix::GetEnv("TERM");
		const bool term_supports_color =
			String::CStringEquals(term, "xterm") ||
			String::CStringEquals(term, "xterm-color") ||
			String::CStringEquals(term, "xterm-256color") ||
			String::CStringEquals(term, "screen") ||
			String::CStringEquals(term, "linux") ||
			String::CStringEquals(term, "cygwin");
		return stdout_is_tty && term_supports_color;
#endif
		return stdout_is_tty;
#endif  // LOGOG_FLAVOR_WINDOWS
	}

	// Helpers for printing colored strings to stdout. Note that on Windows, we
	// cannot simply emit special characters and have the terminal change colors.
	// This routine must actually emit the characters rather than return a string
	// that would be colored when printed, as can be done on Linux.
	void ColoredPrintf(LogogColor color, const char* fmt, ...) 
	{
		va_list args;
		va_start(args, fmt);

#if !defined(LOGOG_FLAVOR_WINDOWS) && !defined(LOGOG_FLAVOR_POSIX)
		const bool use_color = false;
#else
		static const bool in_color_mode =
			ShouldUseColor(true);//isatty(fileno(stdout)) != 0);
		const bool use_color = in_color_mode && (color != COLOR_DEFAULT);
#endif  // !LOGOG_FLAVOR_POSIX && !LOGOG_FLAVOR_WINDOWS
		// The '!= 0' comparison is necessary to satisfy MSVC 7.1.

		if (!use_color) {
			vprintf(fmt, args);
			va_end(args);
			return;
		}

#ifdef LOGOG_FLAVOR_WINDOWS
		const HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);

		// Gets the current text color.
		CONSOLE_SCREEN_BUFFER_INFO buffer_info;
		GetConsoleScreenBufferInfo(stdout_handle, &buffer_info);
		const WORD old_color_attrs = buffer_info.wAttributes;

		// We need to flush the stream buffers into the console before each
		// SetConsoleTextAttribute call lest it affect the text that is already
		// printed but has not yet reached the console.
		fflush(stdout);
		SetConsoleTextAttribute(stdout_handle,
								GetColorAttribute(color) | FOREGROUND_INTENSITY);
		vprintf(fmt, args);

		fflush(stdout);
		// Restores the text color.
		SetConsoleTextAttribute(stdout_handle, old_color_attrs);
#else
		printf("\033[0;3%sm", GetAnsiColorCode(color));
		vprintf(fmt, args);
		printf("\033[m");  // Resets the terminal to default.
#endif  // LOGOG_FLAVOR_WINDOWS
		va_end(args);
	}
	}

	Target::Target() :
		m_bNullTerminatesStrings( true )
	{
		SetFormatter( GetDefaultFormatter() );
		LockableNodesType *pAllTargets = &AllTargets();

		{
			ScopedLock sl( *pAllTargets );
			pAllTargets->insert( this );
		}

		SubscribeToMultiple( AllFilters() );
	}


	Target::~Target()
	{
		LockableNodesType *pAllTargets = &AllTargets();

		UnsubscribeToMultiple( AllFilters() );

		{
			ScopedLock sl( *pAllTargets );
			pAllTargets->erase( this );
		}
	}

	void Target::SetFormatter( Formatter &formatter )
	{
		m_pFormatter = &formatter;
	}

	Formatter & Target::GetFormatter() const
	{
		return *m_pFormatter;
	}

	int Target::Receive( const Topic &topic )
	{
		ScopedLock sl( m_MutexReceive );
		return Output( m_pFormatter->Format( topic, *this ) );
	}

	int Cerr::Output( const LOGOG_STRING &data )
	{
		LOGOG_CERR << (const LOGOG_CHAR *)data;

		return 0;
	}
//! [Cout]
	int Cout::Output( const LOGOG_STRING &data )
	{
		color::ColoredPrintf(color::COLOR_GREEN, (const LOGOG_CHAR *)data);
		//LOGOG_COUT << (const LOGOG_CHAR *)data;

		return 0;
	}
//! [Cout]

	int OutputDebug::Output( const LOGOG_STRING &data )
	{
#ifdef LOGOG_FLAVOR_WINDOWS
#ifdef LOGOG_UNICODE
		OutputDebugStringW( (const LOGOG_CHAR *)data );
#else
		OutputDebugStringA( (const LOGOG_CHAR *)data );
#endif // LOGOG_UNICODE
#endif // LOGOG_FLAVOR_WINDOWS
		return 0;
	}

	LogFile::LogFile(const char *sFileName) :
		m_bFirstTime( true ),
		m_bOpenFailed( false ),
		m_pFile( NULL )
	{
		m_bNullTerminatesStrings = false;

#ifdef LOGOG_UNICODE
		m_bWriteUnicodeBOM = true;
#else // LOGOG_UNICODE
		m_bWriteUnicodeBOM = false;
#endif // LOGOG_UNICODE

		int nNameLength = 0;

		const char *sNameCount = sFileName;
		while ( *sNameCount++ != '\0' )
			nNameLength++;

		// add one for trailing null
		nNameLength++;

		m_pFileName = (char *)Object::Allocate( nNameLength );

		char *m_pOut = m_pFileName;
		while ( ( *m_pOut++ = *sFileName++) != '\0' )
			;
	}

	LogFile::~LogFile()
	{
		if ( m_pFile )
			fclose( m_pFile );

		Object::Deallocate( m_pFileName );
	}

	int LogFile::Open()
	{
		int nError = 1; // preset this in case LOGOG_FLAVOR_WINDOWS is not defined

		bool bFileAlreadyExists = false;
		FILE *fpTest;

#ifdef LOGOG_FLAVOR_WINDOWS
		nError = fopen_s( &fpTest, m_pFileName, "r"); // ignore the error code
#else // LOGOG_FLAVOR_WINDOWS
		fpTest = fopen( m_pFileName, "r");
#endif // LOGOG_FLAVOR_WINDOWS

		if ( fpTest != NULL )
		{
			fclose( fpTest );
			bFileAlreadyExists = true;
		}

		/** Windows tries to be clever and help us with converting line feeds
		 ** to carriage returns when writing a text file.  This causes problems
		 ** when writing a Unicode file as Windows helpfully inserts a single-byte
		 ** 0x0D between the return and line feed on write.  So we open and operate
		 ** the output in binary mode only.
		 **/
#ifdef LOGOG_FLAVOR_WINDOWS
#ifdef LOGOG_UNICODE
		nError = fopen_s( &m_pFile, m_pFileName, "ab, ccs=UNICODE" );
#else // LOGOG_UNICODE
		nError = fopen_s( &m_pFile, m_pFileName, "ab" );
#endif // LOGOG_UNICODE
		if ( nError != 0 )
			return nError;
#else // LOGOG_FLAVOR_WINDOWS
		m_pFile = fopen( m_pFileName, "ab+" );
#endif // LOGOG_FLAVOR_WINDOWS

		if ( m_pFile == NULL )
			m_bOpenFailed = true; // and no further outputs will work
		else
		{
#ifdef LOGOG_UNICODE
			if ( !bFileAlreadyExists )
			{
				WriteUnicodeBOM();
			}
#endif
		}

		return ( m_pFile ? 0 : -1 );
	}

	int LogFile::Output( const LOGOG_STRING &data )
	{
		if ( m_bOpenFailed )
			return -1;

		int result = 0;
		if ( m_bFirstTime )
		{
			result = Open();
			if ( result != 0 )
				return result;

			m_bFirstTime = false;
		}

		return InternalOutput( data.size(), data.c_str());
	}

	int LogFile::InternalOutput( size_t nSize, const LOGOG_CHAR *pData )
		{
        size_t result;

		result = fwrite( pData, sizeof( LOGOG_CHAR ), nSize, m_pFile );

		if ( (size_t)result != nSize )
			return -1;

		return 0;
	}

	void LogFile::WriteUnicodeBOM()
	{
		static union {
			int i;
			char c[4];
		} bDetectEndian = {0x01020304};

		bool bIsLittleEndian = ( bDetectEndian.c[0] != 1 );

		switch ( sizeof( LOGOG_CHAR ))
		{
		case 1:
			// This could be a UTF-8 BOM but technically very few systems support
			// sizeof( wchar_t ) == sizeof( char ).  So for now we're not going
			// to write a BOM in these cases.
			break;

		case 2:
			if ( bIsLittleEndian )
				InternalOutput( 1, (const LOGOG_CHAR *)"\xFF\xFE" ); // little endian UTF-16LE
			else
				InternalOutput( 1, (const LOGOG_CHAR *)"\xFE\xFF" ); // big endian UTF-16BE

			break;

		case 4:
			if ( bIsLittleEndian )
				InternalOutput( 1, (const LOGOG_CHAR *)"\xFF\xFE\x00\x00" ); // little endian UTF-32LE
			else
				InternalOutput( 1, (const LOGOG_CHAR *)"\x00\x00\xFE\xFF" ); // big endian UTF-32BE

			break;

		default:
			// No idea what that character size is; do nothing
			break;
		}
	}

	LogBuffer::LogBuffer( Target *pTarget ,
		size_t s  ) :
	m_pStart( NULL ),
		m_nSize( 0 )
	{
		m_pOutputTarget = pTarget;
		Allocate( s );
	}

	LogBuffer::~LogBuffer()
	{
		Dump();
		Deallocate();
	}

	void LogBuffer::SetTarget( Target &t )
	{
		m_pOutputTarget = &t;
	}

	int LogBuffer::Insert( const LOGOG_CHAR *pChars, size_t size )
	{
		if (( m_pCurrent + size ) >= m_pEnd )
			Dump();

		if ( size > (size_t)( m_pEnd - m_pStart ))
		{
#ifdef LOGOG_INTERNAL_DEBUGGING
			cerr << "Cannot insert string into buffer; string is larger than buffer.  Allocate a larger size for the LogBuffer." << endl;
#endif
			return -1; // can't fit this into buffer; punt it
		}

		// Store the size of this string in the buffer
		size_t *pSize;
		pSize = ( size_t *)m_pCurrent;
		*pSize = size;
		m_pCurrent = (LOGOG_CHAR *)++pSize;

		while ( size-- )
			*m_pCurrent++ = *pChars++;

		return 0;
	}

	int LogBuffer::Dump()
	{
		LOGOG_CHAR *pCurrent = m_pStart;
		size_t *pSize;
		int nError;

		if ( m_pOutputTarget == NULL )
			return -1;

		// We have to lock the output target here, as we do an end run around its Receive() function */
		ScopedLock sl( m_pOutputTarget->m_MutexReceive );

		while ( pCurrent < m_pCurrent )
		{
			String sOut;
			// Get the size of this entry
			pSize = ( size_t * )pCurrent;
			// Move past that entry into the data area
			pCurrent = ( LOGOG_CHAR * )( pSize + 1 );

			sOut.assign( pCurrent, pCurrent + *pSize - 1 );

			if ( m_pOutputTarget )
			{
				nError = m_pOutputTarget->Output( sOut );
				if ( nError != 0 )
					return nError;
			}

			pCurrent += *pSize;
		}

		// reset buffer
		m_pCurrent = m_pStart;

		return 0;
	}

	int LogBuffer::Output( const LOGOG_STRING &data )
	{
		return Insert( &(*data), data.size() );
	}

	void LogBuffer::Allocate( size_t size )
	{
		m_nSize = size;
		m_pCurrent = m_pStart = (LOGOG_CHAR *)Object::Allocate( size * sizeof( LOGOG_CHAR ));
		m_pEnd = m_pStart + size;
	}

	void LogBuffer::Deallocate()
	{
		if ( m_pStart )
			Object::Deallocate( m_pStart );

		m_nSize = 0;
	}
}
