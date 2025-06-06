/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

/// \file
/// C-style null-terminated-character-array string library.

#include <cstring>
#include <cctype>
#include <algorithm>
#include <utility>

#include "memory/allocator.h"
#include "generic/arrayrange.h"

/// \brief Returns true if \p string length is zero.
/// O(1)
inline bool string_empty( const char* string ){
	return *string == '\0';
}

/// \brief Returns true if \p string length is not zero.
/// O(1)
inline bool string_not_empty( const char* string ){
	return !string_empty( string );
}

/// \brief Returns <0 if \p string is lexicographically less than \p other.
/// Returns >0 if \p string is lexicographically greater than \p other.
/// Returns 0 if \p string is lexicographically equal to \p other.
/// O(n)
inline int string_compare( const char* string, const char* other ){
	return std::strcmp( string, other );
}

/// \brief Returns <0 if [\p string, \p string + \p n) is lexicographically less than [\p other, \p other + \p n).
/// Returns >0 if [\p string, \p string + \p n) is lexicographically greater than [\p other, \p other + \p n).
/// Returns 0 if [\p string, \p string + \p n) is lexicographically equal to [\p other, \p other + \p n).
/// O(n)
inline int string_compare_n( const char* string, const char* other, std::size_t n ){
	return std::strncmp( string, other, n );
}

/// \brief Returns true if \p string is lexicographically equal to \p other.
/// O(n)
inline bool string_equal( const char* string, const char* other ){
	return string_compare( string, other ) == 0;
}

/// \brief Returns true if [\p string, \p string + \p n) is lexicographically equal to [\p other, \p other + \p n).
/// O(n)
inline bool string_equal_n( const char* string, const char* other, std::size_t n ){
	return string_compare_n( string, other, n ) == 0;
}

/// \brief Returns true if \p string is lexicographically less than \p other.
/// O(n)
inline bool string_less( const char* string, const char* other ){
	return string_compare( string, other ) < 0;
}

/// \brief Returns true if \p string is lexicographically greater than \p other.
/// O(n)
inline bool string_greater( const char* string, const char* other ){
	return string_compare( string, other ) > 0;
}

/// \brief Returns <0 if \p string is lexicographically less than \p other after converting both to lower-case.
/// Returns >0 if \p string is lexicographically greater than \p other after converting both to lower-case.
/// Returns 0 if \p string is lexicographically equal to \p other after converting both to lower-case.
/// O(n)
inline int string_compare_nocase( const char* string, const char* other ){
#ifdef WIN32
	return _stricmp( string, other );
#else
	return strcasecmp( string, other );
#endif
}

/// \brief Returns <0 if [\p string, \p string + \p n) is lexicographically less than [\p other, \p other + \p n).
/// Returns >0 if [\p string, \p string + \p n) is lexicographically greater than [\p other, \p other + \p n).
/// Returns 0 if [\p string, \p string + \p n) is lexicographically equal to [\p other, \p other + \p n).
/// Treats all ascii characters as lower-case during comparisons.
/// O(n)
inline int string_compare_nocase_n( const char* string, const char* other, std::size_t n ){
#ifdef WIN32
	return _strnicmp( string, other, n );
#else
	return strncasecmp( string, other, n );
#endif
}

/// \brief Returns true if \p string is lexicographically equal to \p other.
/// Treats all ascii characters as lower-case during comparisons.
/// O(n)
inline bool string_equal_nocase( const char* string, const char* other ){
	return string_compare_nocase( string, other ) == 0;
}

/// \brief Returns true if [\p string, \p string + \p n) is lexicographically equal to [\p other, \p other + \p n).
/// Treats all ascii characters as lower-case during comparisons.
/// O(n)
inline bool string_equal_nocase_n( const char* string, const char* other, std::size_t n ){
	return string_compare_nocase_n( string, other, n ) == 0;
}

/// \brief Returns true if \p string is lexicographically less than \p other.
/// Treats all ascii characters as lower-case during comparisons.
/// O(n)
inline bool string_less_nocase( const char* string, const char* other ){
	return string_compare_nocase( string, other ) < 0;
}

/// \brief Returns true if \p string is lexicographically greater than \p other.
/// Treats all ascii characters as lower-case during comparisons.
/// O(n)
inline bool string_greater_nocase( const char* string, const char* other ){
	return string_compare_nocase( string, other ) > 0;
}

/*!
   This behaves identically to stricmp(a,b), except that ASCII chars
   [\]^`_ come AFTER alphabet chars instead of before. This is because
   it converts all alphabet chars to uppercase before comparison,
   while stricmp converts them to lowercase.
 */
inline int string_compare_nocase_upper( const char* a, const char* b ){
	for (;; )
	{
		const int c1 = std::toupper( *a++ );
		const int c2 = std::toupper( *b++ );

		if ( c1 < c2 )
			return -1; // a < b
		if ( c1 > c2 )
			return 1; // a > b
		if ( c1 == 0 )
			return 0; // a == b
	}
}

/// \brief Returns the number of non-null characters in \p string.
/// O(n)
inline std::size_t string_length( const char* string ){
	return std::strlen( string );
}

/// \brief Returns true if the beginning of \p string is equal to \p prefix.
/// O(n)
inline bool string_equal_prefix( const char* string, const char* prefix ){
	return string_equal_n( string, prefix, string_length( prefix ) );
}

inline bool string_equal_prefix_nocase( const char* string, const char* prefix ){
	return string_equal_nocase_n( string, prefix, string_length( prefix ) );
}

/// \brief Returns true if the ending of \p string is equal to \p suffix.
/// O(n)
inline bool string_equal_suffix( const char* string, const char* suffix ){
	const std::size_t stringLength = string_length( string );
	const std::size_t suffixLength = string_length( suffix );
	return ( suffixLength > stringLength )? false : string_equal_n( string + stringLength - suffixLength, suffix, suffixLength );
}

inline bool string_equal_suffix_nocase( const char* string, const char* suffix ){
	const std::size_t stringLength = string_length( string );
	const std::size_t suffixLength = string_length( suffix );
	return ( suffixLength > stringLength )? false : string_equal_nocase_n( string + stringLength - suffixLength, suffix, suffixLength );
}

/// \brief Copies \p other into \p string and returns \p string.
/// Assumes that the space allocated for \p string is at least string_length(other) + 1.
/// O(n)
inline char* string_copy( char* string, const char* other ){
	return std::strcpy( string, other );
}

/// \brief Allocates a string buffer large enough to hold \p length characters, using \p allocator.
/// The returned buffer must be released with \c string_release using a matching \p allocator.
template<typename Allocator>
inline char* string_new( std::size_t length, Allocator& allocator ){
	return allocator.allocate( length + 1 );
}

/// \brief Deallocates the \p buffer large enough to hold \p length characters, using \p allocator.
template<typename Allocator>
inline void string_release( char* buffer, std::size_t length, Allocator& allocator ){
	allocator.deallocate( buffer, length + 1 );
}

/// \brief Returns a newly-allocated string which is a clone of \p other, using \p allocator.
/// The returned buffer must be released with \c string_release using a matching \p allocator.
template<typename Allocator>
inline char* string_clone( const char* other, Allocator& allocator ){
	char* copied = string_new( string_length( other ), allocator );
	std::strcpy( copied, other );
	return copied;
}

/// \brief Returns a newly-allocated string which is a clone of [\p first, \p last), using \p allocator.
/// The returned buffer must be released with \c string_release using a matching \p allocator.
template<typename Allocator>
inline char* string_clone_range( StringRange range, Allocator& allocator ){
	char* copied = string_new( range.size(), allocator );
	*std::copy_n( range.data(), range.size(), copied ) = '\0';
	return copied;
}

/// \brief Allocates a string buffer large enough to hold \p length characters.
/// The returned buffer must be released with \c string_release.
inline char* string_new( std::size_t length ){
	DefaultAllocator<char> allocator;
	return string_new( length, allocator );
}

/// \brief Deallocates the \p buffer large enough to hold \p length characters.
inline void string_release( char* string, std::size_t length ){
	DefaultAllocator<char> allocator;
	string_release( string, length, allocator );
}

/// \brief Returns a newly-allocated string which is a clone of \p other.
/// The returned buffer must be released with \c string_release.
inline char* string_clone( const char* other ){
	DefaultAllocator<char> allocator;
	return string_clone( other, allocator );
}

/// \brief Returns a newly-allocated string which is a clone of [\p first, \p last).
/// The returned buffer must be released with \c string_release.
inline char* string_clone_range( StringRange range ){
	DefaultAllocator<char> allocator;
	return string_clone_range( range, allocator );
}

typedef char* char_pointer;
/// \brief Swaps the values of \p string and \p other.
inline void string_swap( char_pointer& string, char_pointer& other ){
	std::swap( string, other );
}

typedef const char* char_const_pointer;
/// \brief Swaps the values of \p string and \p other.
inline void string_swap( char_const_pointer& string, char_const_pointer& other ){
	std::swap( string, other );
}

/// \brief Converts each character of \p string to lower-case and returns \p string.
/// O(n)
inline char* string_to_lowercase( char* string ){
	for ( char* p = string; *p != '\0'; ++p )
	{
		*p = (char)std::tolower( *p );
	}
	return string;
}

/// \brief Converts each character of \p string to upper-case and returns \p string.
/// O(n)
inline char* string_to_uppercase( char* string ){
	for ( char* p = string; *p != '\0'; ++p )
	{
		*p = (char)std::toupper( *p );
	}
	return string;
}

//http://stackoverflow.com/questions/27303062/strstr-function-like-that-ignores-upper-or-lower-case
//chux: Somewhat tricky to match the corner cases of strstr() with inputs like "x","", "","x", "",""
inline const char* string_in_string_nocase( const char* haystack, const char* needle ) {
	do {
		const char* h = haystack;
		const char* n = needle;
		while ( std::tolower( ( unsigned char ) *h ) == std::tolower( ( unsigned char ) *n ) && *n ) {
			h++;
			n++;
		}
		if ( *n == 0 ) {
			return haystack;
		}
	} while ( *haystack++ );
	return 0;
}

/// \brief A re-entrant string tokeniser similar to strchr.
class StringTokeniser
{
	bool istoken( char c ) const {
		if ( strchr( m_delimiters, c ) != 0 ) {
			return false;
		}
		return true;
	}
	const char* advance(){
		const char* token = m_pos;
		bool intoken = true;
		while ( !string_empty( m_pos ) )
		{
			if ( !istoken( *m_pos ) ) {
				*m_pos = '\0';
				intoken = false;
			}
			else if ( !intoken ) {
				return token;
			}
			++m_pos;
		}
		return token;
	}
	std::size_t m_length;
	char* m_string;
	char* m_pos;
	const char* m_delimiters;
public:
	StringTokeniser( const char* string, const char* delimiters = " \n\r\t\v" ) :
		m_length( string_length( string ) ),
		m_string( string_copy( string_new( m_length ), string ) ),
		m_pos( m_string ),
		m_delimiters( delimiters ){
		while ( !string_empty( m_pos ) && !istoken( *m_pos ) )
		{
			++m_pos;
		}
	}
	~StringTokeniser(){
		string_release( m_string, m_length );
	}
	/// \brief Returns the next token or "" if there are no more tokens available.
	const char* getToken(){
		return advance();
	}
};

/// \brief A non-mutable c-style string.
///
/// \param Buffer The string storage implementation. Must be DefaultConstructible, CopyConstructible and Assignable. Must implement:
/// \li Buffer(const char* string) - constructor which copies a c-style \p string.
/// \li Buffer(const char* first, const char*) - constructor which copies a c-style string range [\p first, \p last).
/// \li void swap(Buffer& other) - swaps contents with \p other.
/// \li const char* c_str() - returns the stored non-mutable c-style string.
template<typename Buffer>
class String : public Buffer
{
public:

	String()
		: Buffer(){
	}
	String( const char* string )
		: Buffer( string ){
	}
	String( StringRange range )
		: Buffer( range ){
	}
	String( const String& other )
		: Buffer( other ){
	}
	String( String&& other ) noexcept
		: Buffer( std::move( other ) ) {
	}

	String& operator=( const String& other ){
		String temp( other );
		temp.swap( *this );
		return *this;
	}
	String& operator=( String&& other ) noexcept {
		swap( other );
		return *this;
	}
	String& operator=( const char* string ){
		String temp( string );
		temp.swap( *this );
		return *this;
	}
	String& operator=( StringRange range ){
		String temp( range );
		temp.swap( *this );
		return *this;
	}

	void swap( String& other ){
		Buffer::swap( other );
	}

	bool empty() const {
		return string_empty( Buffer::c_str() );
	}
};

template<typename Buffer>
inline bool operator<( const String<Buffer>& self, const String<Buffer>& other ){
	return string_less( self.c_str(), other.c_str() );
}

template<typename Buffer>
inline bool operator>( const String<Buffer>& self, const String<Buffer>& other ){
	return string_greater( self.c_str(), other.c_str() );
}

template<typename Buffer>
inline bool operator==( const String<Buffer>& self, const String<Buffer>& other ){
	return string_equal( self.c_str(), other.c_str() );
}

template<typename Buffer>
inline bool operator!=( const String<Buffer>& self, const String<Buffer>& other ){
	return !string_equal( self.c_str(), other.c_str() );
}

template<typename Buffer>
inline bool operator==( const String<Buffer>& self, const char* other ){
	return string_equal( self.c_str(), other );
}

template<typename Buffer>
inline bool operator!=( const String<Buffer>& self, const char* other ){
	return !string_equal( self.c_str(), other );
}

namespace std
{
/// \brief Swaps the values of \p self and \p other.
/// Overloads std::swap.
template<typename Buffer>
inline void swap( String<Buffer>& self, String<Buffer>& other ){
	self.swap( other );
}
}


/// \brief A non-mutable string buffer which manages memory allocation.
template<typename Allocator>
class CopiedBuffer : private Allocator
{
	char* m_string;

	char* copy_range( StringRange range ){
		return string_clone_range( range, static_cast<Allocator&>( *this ) );
	}
	char* copy( const char* other ){
		return string_clone( other, static_cast<Allocator&>( *this ) );
	}
	void destroy( char* string ){
		string_release( string, string_length( string ), static_cast<Allocator&>( *this ) );
	}

protected:
	~CopiedBuffer(){
		destroy( m_string );
	}
public:
	CopiedBuffer()
		: m_string( copy( "" ) ){
	}
	explicit CopiedBuffer( const Allocator& allocator )
		: Allocator( allocator ), m_string( copy( "" ) ){
	}
	CopiedBuffer( const CopiedBuffer& other )
		: Allocator( other ), m_string( copy( other.m_string ) ){
	}
	CopiedBuffer( const char* string, const Allocator& allocator = Allocator() )
		: Allocator( allocator ), m_string( copy( string ) ){
	}
	CopiedBuffer( StringRange range, const Allocator& allocator = Allocator() )
		: Allocator( allocator ), m_string( copy_range( range ) ){
	}
	const char* c_str() const {
		return m_string;
	}
	void swap( CopiedBuffer& other ){
		string_swap( m_string, other.m_string );
	}
};

/// \brief A non-mutable string buffer which manages memory allocation.
class DefaultCopiedBuffer
{
	char* m_string;

	char* copy_range( StringRange range ){
		return string_clone_range( range );
	}
	char* copy( const char* other ){
		return string_clone( other );
	}
	void destroy( char* string ){
		string_release( string, 0 ); // DefaultAllocator needs no length, strlen(nullprt) doesn't work either
	}

protected:
	~DefaultCopiedBuffer(){
		destroy( m_string );
	}
public:
	DefaultCopiedBuffer()
		: m_string( copy( "" ) ){
	}
	DefaultCopiedBuffer( const DefaultCopiedBuffer& other )
		: m_string( copy( other.m_string ) ){
	}
	DefaultCopiedBuffer( DefaultCopiedBuffer&& other ) noexcept
		: m_string( std::exchange( other.m_string, nullptr ) ){ // other will be destroyed fairly soon, operator delete(nullptr) is cool
	}
	DefaultCopiedBuffer( const char* string )
		: m_string( copy( string ) ){
	}
	DefaultCopiedBuffer( StringRange range )
		: m_string( copy_range( range ) ){
	}
	const char* c_str() const {
		return m_string;
	}
	void swap( DefaultCopiedBuffer& other ){
		string_swap( m_string, other.m_string );
	}
};

/// \brief A non-mutable string which uses copy-by-value for assignment.
typedef String< DefaultCopiedBuffer > CopiedString;
// typedef String< CopiedBuffer< DefaultAllocator<char> > > CopiedString;


/// \brief Writes CopiedString \p string to \p ostream.
template<typename TextOutputStreamType>
inline TextOutputStreamType& ostream_write( TextOutputStreamType& ostream, const CopiedString& string ){
	ostream.write( string.c_str(), string_length( string.c_str() ) );
	return ostream;
}


/// \brief A non-mutable string buffer which uses reference-counting to avoid unnecessary allocations.
template<typename Allocator>
class SmartBuffer : private Allocator
{
	char* m_buffer;

	char* copy_range( StringRange range ){
		char* buffer = Allocator::allocate( sizeof( std::size_t ) + range.size() + 1 );
		*std::copy_n( range.data(), range.size(), buffer + sizeof( std::size_t ) ) = '\0';
		*reinterpret_cast<std::size_t*>( buffer ) = 0;
		return buffer;
	}
	char* copy( const char* string ){
		char* buffer = Allocator::allocate( sizeof( std::size_t ) + string_length( string ) + 1 );
		strcpy( buffer + sizeof( std::size_t ), string );
		*reinterpret_cast<std::size_t*>( buffer ) = 0;
		return buffer;
	}
	void destroy( char* buffer ){
		Allocator::deallocate( buffer, sizeof( std::size_t ) + string_length( c_str() ) + 1 );
	}

	void incref( char* buffer ){
		++( *reinterpret_cast<std::size_t*>( buffer ) );
	}
	void decref( char* buffer ){
		if ( --( *reinterpret_cast<std::size_t*>( buffer ) ) == 0 ) {
			destroy( buffer );
		}
	}

protected:
	~SmartBuffer(){
		decref( m_buffer );
	}
public:
	SmartBuffer()
		: m_buffer( copy( "" ) ){
		incref( m_buffer );
	}
	explicit SmartBuffer( const Allocator& allocator )
		: Allocator( allocator ), m_buffer( copy( "" ) ){
		incref( m_buffer );
	}
	SmartBuffer( const SmartBuffer& other )
		: Allocator( other ), m_buffer( other.m_buffer ){
		incref( m_buffer );
	}
	SmartBuffer( const char* string, const Allocator& allocator = Allocator() )
		: Allocator( allocator ), m_buffer( copy( string ) ){
		incref( m_buffer );
	}
	SmartBuffer( StringRange range, const Allocator& allocator = Allocator() )
		: Allocator( allocator ), m_buffer( copy_range( range ) ){
		incref( m_buffer );
	}
	const char* c_str() const {
		return m_buffer + sizeof( std::size_t );
	}
	void swap( SmartBuffer& other ){
		string_swap( m_buffer, other.m_buffer );
	}
};

/// \brief A non-mutable string which uses copy-by-reference for assignment of SmartString.
typedef String< SmartBuffer< DefaultAllocator<char> > > SmartString;


template<>
struct std::less<CopiedString>
{
	using is_transparent = void;

	bool operator()( const CopiedString& x, const CopiedString& y ) const {
		return string_less( x.c_str(), y.c_str() );
	}
	bool operator()( const CopiedString& x, const char *y ) const {
		return string_less( x.c_str(), y );
	}
	bool operator()( const char *x, const CopiedString& y ) const {
		return string_less( x, y.c_str() );
	}
	bool operator()( const CopiedString& string, const StringRange range ) const {
		return string_compare_n( string.c_str(), range.data(), range.size() ) < 0;
	}
	bool operator()( const StringRange range, const CopiedString& string ) const {
		return string_compare_n( string.c_str(), range.data(), range.size() ) > 0;
	}
};


class StringEqualNoCase
{
public:
	bool operator()( const CopiedString& key, const CopiedString& other ) const {
		return string_equal_nocase( key.c_str(), other.c_str() );
	}
};

struct StringLessNoCase
{
	using is_transparent = void;

	bool operator()( const CopiedString& x, const CopiedString& y ) const {
		return string_less_nocase( x.c_str(), y.c_str() );
	}
	bool operator()( const CopiedString& x, const char *y ) const {
		return string_less_nocase( x.c_str(), y );
	}
	bool operator()( const char *x, const CopiedString& y ) const {
		return string_less_nocase( x, y.c_str() );
	}
	bool operator()( const CopiedString& string, const StringRange range ) const {
		return string_compare_nocase_n( string.c_str(), range.data(), range.size() ) < 0;
	}
	bool operator()( const StringRange range, const CopiedString& string ) const {
		return string_compare_nocase_n( string.c_str(), range.data(), range.size() ) > 0;
	}
};

struct RawStringEqual
{
	bool operator()( const char* x, const char* y ) const {
		return string_equal( x, y );
	}
};

struct RawStringLess
{
	bool operator()( const char* x, const char* y ) const {
		return string_less( x, y );
	}
};

struct RawStringLessNoCase
{
	bool operator()( const char* x, const char* y ) const {
		return string_less_nocase( x, y );
	}
};
