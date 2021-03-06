/* 
   Unix SMB/Netbios implementation.
   Version 1.9.
   Name mangling
   Copyright (C) Andrew Tridgell 1992-1998
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* -------------------------------------------------------------------------- **
 * Notable problems...
 *
 *  March/April 1998  CRH
 *  - Many of the functions in this module overwrite string buffers passed to
 *    them.  This causes a variety of problems and is, generally speaking,
 *    dangerous and scarry.  See the kludge notes in name_map_mangle()
 *    below.
 *  - It seems that something is calling name_map_mangle() twice.  The
 *    first call is probably some sort of test.  Names which contain
 *    illegal characters are being doubly mangled.  I'm not sure, but
 *    I'm guessing the problem is in server.c.
 *
 * -------------------------------------------------------------------------- **
 */

/* -------------------------------------------------------------------------- **
 * History...
 *
 *  March/April 1998  CRH
 *  Updated a bit.  Rewrote is_mangled() to be a bit more selective.
 *  Rewrote the mangled name cache.  Added comments here and there.
 *  &c.
 * -------------------------------------------------------------------------- **
 */

#include "includes.h"


/* -------------------------------------------------------------------------- **
 * External Variables...
 */

extern int case_default;    /* Are conforming 8.3 names all upper or lower?   */
extern BOOL case_mangle;    /* If true, all chars in 8.3 should be same case. */

/* -------------------------------------------------------------------------- **
 * Other stuff...
 *
 * magic_char     - This is the magic char used for mangling.  It's
 *                  global.  There is a call to lp_magicchar() in server.c
 *                  that is used to override the initial value.
 *
 * MANGLE_BASE    - This is the number of characters we use for name mangling.
 *
 * basechars      - The set characters used for name mangling.  This
 *                  is static (scope is this file only).
 *
 * mangle()       - Macro used to select a character from basechars (i.e.,
 *                  mangle(n) will return the nth digit, modulo MANGLE_BASE).
 *
 * chartest       - array 0..255.  The index range is the set of all possible
 *                  values of a byte.  For each byte value, the content is a
 *                  two nibble pair.  See BASECHAR_MASK and ILLEGAL_MASK,
 *                  below.
 *
 * ct_initialized - False until the chartest array has been initialized via
 *                  a call to init_chartest().
 *
 * BASECHAR_MASK  - Masks the upper nibble of a one-byte value.
 *
 * ILLEGAL_MASK   - Masks the lower nibble of a one-byte value.
 *
 * isbasecahr()   - Given a character, check the chartest array to see
 *                  if that character is in the basechars set.  This is
 *                  faster than using strchr().
 *
 * isillegal()    - Given a character, check the chartest array to see
 *                  if that character is in the illegal characters set.
 *                  This is faster than using strchr().
 *
 * mangled_cache  - Cache header used for storing mangled -> original
 *                  reverse maps.
 *
 * mc_initialized - False until the mangled_cache structure has been
 *                  initialized via a call to reset_mangled_cache().
 *
 * MANGLED_CACHE_MAX_ENTRIES - Default maximum number of entries for the
 *                  cache.  A value of 0 indicates "infinite".
 *
 * MANGLED_CACHE_MAX_MEMORY  - Default maximum amount of memory for the
 *                  cache.  When the cache was kept as an array of 256
 *                  byte strings, the default cache size was 50 entries.
 *                  This required a fixed 12.5Kbytes of memory.  The
 *                  mangled stack parameter is no longer used (though
 *                  this might change).  We're now using a fixed 16Kbyte
 *                  maximum cache size.  This will probably be much more
 *                  than 50 entries.
 */

char magic_char = '~';

static char basechars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_-!@#$%";
#define MANGLE_BASE       (sizeof(basechars)/sizeof(char)-1)

static unsigned char chartest[256]  = { 0 };
static BOOL          ct_initialized = False;

#define mangle(V) ((char)(basechars[(V) % MANGLE_BASE]))
#define BASECHAR_MASK 0xf0
#define ILLEGAL_MASK  0x0f
#define isbasechar(C) ( (chartest[ ((C) & 0xff) ]) & BASECHAR_MASK )
#define isillegal(C) ( (chartest[ ((C) & 0xff) ]) & ILLEGAL_MASK )

static ubi_cacheRoot mangled_cache[1] =  { { { 0, 0, 0, 0 }, 0, 0, 0, 0, 0, 0 } };
static BOOL          mc_initialized   = False;
#define MANGLED_CACHE_MAX_ENTRIES 1024
#define MANGLED_CACHE_MAX_MEMORY  0


/* -------------------------------------------------------------------------- **
 * Functions...
 */

/* ************************************************************************** **
 * Initialize the static character test array.
 *
 *  Input:  none
 *
 *  Output: none
 *
 *  Notes:  This function changes (loads) the contents of the <chartest>
 *          array.  The scope of <chartest> is this file.
 *
 * ************************************************************************** **
 */
static void init_chartest( void )
  {
  const char          *illegalchars = "*\\/?<>|\":";
  const unsigned char *s;
  
  memset( (char *)chartest, '\0', 256 );

  for( s = (const unsigned char *)illegalchars; *s; s++ )
    chartest[*s] = ILLEGAL_MASK;

  for( s = (const unsigned char *)basechars; *s; s++ )
    chartest[*s] |= BASECHAR_MASK;

  ct_initialized = True;
  } /* init_chartest */

/* ************************************************************************** **
 * Return True if a name is a special msdos reserved name.
 *
 *  Input:  fname - String containing the name to be tested.
 *
 *  Output: True, if the name matches one of the list of reserved names.
 *
 *  Notes:  This is a static function called by is_8_3(), below.
 *
 * ************************************************************************** **
 */
static BOOL is_reserved_msdos( const char *fname )
  {
  char upperFname[13];
  char *p;

  StrnCpy (upperFname, fname, 12);

  /* lpt1.txt and con.txt etc are also illegal */
  p = strchr(upperFname,'.');
  if( p )
    *p = '\0';

  strupper( upperFname );
  p = upperFname + 1;
  switch( upperFname[0] )
    {
    case 'A':
      if( 0 == strcmp( p, "UX" ) )
        return( True );
      break;
    case 'C':
      if( (0 == strcmp( p, "LOCK$" ))
       || (0 == strcmp( p, "ON" ))
       || (0 == strcmp( p, "OM1" ))
       || (0 == strcmp( p, "OM2" ))
       || (0 == strcmp( p, "OM3" ))
       || (0 == strcmp( p, "OM4" ))
        )
        return( True );
      break;
    case 'L':
      if( (0 == strcmp( p, "PT1" ))
       || (0 == strcmp( p, "PT2" ))
       || (0 == strcmp( p, "PT3" ))
        )
        return( True );
      break;
    case 'N':
      if( 0 == strcmp( p, "UL" ) )
        return( True );
      break;
    case 'P':
      if( 0 == strcmp( p, "RN" ) )
        return( True );
      break;
    }

  return( False );
  } /* is_reserved_msdos */

/* ************************************************************************** **
 * Determine whether or not a given name contains illegal characters, even
 * long names.
 *
 *  Input:  name  - The name to be tested.
 *
 *  Output: True if an illegal character was found in <name>, else False.
 *
 *  Notes:  This is used to test a name on the host system, long or short,
 *          for characters that would be illegal on most client systems,
 *          particularly DOS and Windows systems.  Unix and AmigaOS, for
 *          example, allow a filenames which contain such oddities as
 *          quotes (").  If a name is found which does contain an illegal
 *          character, it is mangled even if it conforms to the 8.3
 *          format.
 *
 * ************************************************************************** **
 */
static BOOL is_illegal_name( char *name )
  {
  unsigned char *s;
  int            skip;
  int namelen;

  if( !name )
    return( True );

  if( !ct_initialized )
    init_chartest();

  namelen = strlen(name);
  if (namelen &&
		  name[namelen-1] == '.' &&
		  !strequal(name, ".") &&
		  !strequal(name, ".."))
	  return True;

  s = (unsigned char *)name;
  while( *s )
    {
    skip = get_character_len( *s );
    if( skip != 0 )
      {
      s += skip;
      }
    else
      {
      if( isillegal( *s ) )
        return( True );
      else
        s++;
      }
    }

  return( False );
  } /* is_illegal_name */

/* ************************************************************************** **
 * Return True if the name *could be* a mangled name.
 *
 *  Input:  s - A path name - in UNIX pathname format.
 *
 *  Output: True if the name matches the pattern described below in the
 *          notes, else False.
 *
 *  Notes:  The input name is *not* tested for 8.3 compliance.  This must be
 *          done separately.  This function returns true if the name contains
 *          a magic character followed by excactly two characters from the
 *          basechars list (above), which in turn are followed either by the
 *          nul (end of string) byte or a dot (extension) or by a '/' (end of
 *          a directory name).
 *
 * ************************************************************************** **
 */
static BOOL is_mangled( const char *s )
{
	char *magic;
	BOOL ret = False;

	if( !ct_initialized )
		init_chartest();

	magic = strchr( s, magic_char );
	while( magic && magic[1] && magic[2] ) {
		/* 3 chars, 1st is magic. */
		if( ('.' == magic[3] || '/' == magic[3] || !(magic[3]))          /* Ends with '.' or nul or '/' ?  */
				&& isbasechar( toupper(magic[1]) )           /* is 2nd char basechar?  */
				&& isbasechar( toupper(magic[2]) ) )         /* is 3rd char basechar?  */
			ret = ( True );                           /* If all above, then true, */
		magic = strchr( magic+1, magic_char );      /*    else seek next magic. */
	}

	DEBUG(10,("is_mangled: %s : %s\n", s, ret ? "True" : "False"));

	return ret;
} /* is_mangled */

static BOOL is_ms_wildchar(const char c)
{
	switch(c) {
		case '*':
		case '?':
		case '<':
		case '>':
		case '"':
			return True;
		default:
			return False;
	}
}

/* ************************************************************************** **
 * Return True if the name is a valid DOS name in 8.3 DOS format.
 *
 *  Input:  fname       - File name to be checked.
 *          check_case  - If True, and if case_mangle is True, then the
 *                        name will be checked to see if all characters
 *                        are the correct case.  See case_mangle and
 *                        case_default above.
 *          allow_wildcards - If True dos wildcard characters *?<>" are allowed as 8.3.
 *
 *  Output: True if the name is a valid DOS name, else False.
 *
 * ************************************************************************** **
 */

static BOOL is_8_3( const char *cfname, BOOL check_case, BOOL allow_wildcards )
{
	int   len;
	int   l;
	int   skip;
	const char *p;
	const char *dot_pos;
	char *slash_pos;
	const char *fname = cfname;

	slash_pos = strrchr( fname, '/' );

	/* If there is a directory path, skip it. */
	if( slash_pos )
		fname = slash_pos + 1;
	len = strlen( fname );

	DEBUG( 5, ( "Checking %s for 8.3\n", fname ) );

	/* Can't be 0 chars or longer than 12 chars */
	if( (len == 0) || (len > 12) )
		return( False );

	/* Mustn't be an MS-DOS Special file such as lpt1 or even lpt1.txt */
	if( is_reserved_msdos( fname ) )
		return( False );

	/* Check that all characters are the correct case, if asked to do so. */
	if( check_case && case_mangle ) {
		switch( case_default ) {
			case CASE_LOWER:
				if( strhasupper( fname ) )
					return(False);
				break;
			case CASE_UPPER:
				if( strhaslower( fname ) )
					return(False);
				break;
			}
	}

	/* Can't contain invalid dos chars */
	/* Windows use the ANSI charset.
		But filenames are translated in the PC charset.
		This Translation may be more or less relaxed depending
		the Windows application. */

	/* %%% A nice improvment to name mangling would be to translate
		filename to ANSI charset on the smb server host */

	p       = fname;
	dot_pos = NULL;
	while( *p ) {
		if( (skip = get_character_len( *p )) != 0 )
			p += skip;
		else {
			if( *p == '.' && !dot_pos )
				dot_pos = p;
			else {
				if( !isdoschar( *p )) {
					if (!allow_wildcards)
						return False;
					if (!is_ms_wildchar(*p))
						return False;
				}
			}
			p++;
		}
	}

	/* no dot and less than 9 means OK */
	if( !dot_pos )
		return( len <= 8 );
        
	l = PTR_DIFF( dot_pos, fname );

	/* base must be at least 1 char except special cases . and .. */
	if( l == 0 )
		return( 0 == strcmp( fname, "." ) || 0 == strcmp( fname, ".." ) );

	/* base can't be greater than 8 */
	if( l > 8 )
		return( False );

	/* extension must be between 1 and 3 */
	if( (len - l < 2 ) || (len - l > 4) )
		return( False );

	/* extensions may not have a dot */
	if( strchr( dot_pos+1, '.' ) )
		return( False );

	/* must be in 8.3 format */
	return( True );
}

/* ************************************************************************** **
 * Compare two cache keys and return a value indicating their ordinal
 * relationship.
 *
 *  Input:  ItemPtr - Pointer to a comparison key.  In this case, this will
 *                    be a mangled name string.
 *          NodePtr - Pointer to a node in the cache.  The node structure
 *                    will be followed in memory by a mangled name string.
 *
 *  Output: A signed integer, as follows:
 *            (x < 0)  <==> Key1 less than Key2
 *            (x == 0) <==> Key1 equals Key2
 *            (x > 0)  <==> Key1 greater than Key2
 *
 *  Notes:  This is a ubiqx-style comparison routine.  See ubi_BinTree for
 *          more info.
 *
 * ************************************************************************** **
 */

static signed int cache_compare( ubi_btItemPtr ItemPtr, ubi_btNodePtr NodePtr )
{
	char *Key1 = (char *)ItemPtr;
	char *Key2 = (char *)(((ubi_cacheEntryPtr)NodePtr) + 1);

	DEBUG(100,("cache_compare: %s %s\n", Key1, Key2));

	return( StrCaseCmp( Key1, Key2 ) );
}

/* ************************************************************************** **
 * Free a cache entry.
 *
 *  Input:  WarrenZevon - Pointer to the entry that is to be returned to
 *                        Nirvana.
 *  Output: none.
 *
 *  Notes:  This function gets around the possibility that the standard
 *          free() function may be implemented as a macro, or other evil
 *          subversions (oh, so much fun).
 *
 * ************************************************************************** **
 */

static void cache_free_entry( ubi_trNodePtr WarrenZevon )
{
	ZERO_STRUCTP(WarrenZevon);
	SAFE_FREE( WarrenZevon );
}

/* ************************************************************************** **
 * Initializes or clears the mangled cache.
 *
 *  Input:  none.
 *  Output: none.
 *
 *  Notes:  There is a section below that is commented out.  It shows how
 *          one might use lp_ calls to set the maximum memory and entry size
 *          of the cache.  You might also want to remove the constants used
 *          in ubi_cacheInit() and replace them with lp_ calls.  If so, then
 *          the calls to ubi_cacheSetMax*() would be moved into the else
 *          clause.  Another option would be to pass in the max_entries and
 *          max_memory values as parameters.  crh 09-Apr-1998.
 *
 * ************************************************************************** **
 */

static void reset_mangled_cache( void )
{
	if( !mc_initialized ) {
		(void)ubi_cacheInit( mangled_cache,
							cache_compare,
							cache_free_entry,
							MANGLED_CACHE_MAX_ENTRIES,
							MANGLED_CACHE_MAX_MEMORY );
		mc_initialized = True;
	} else {
		(void)ubi_cacheClear( mangled_cache );
	}

	/*
		(void)ubi_cacheSetMaxEntries( mangled_cache, lp_mangled_cache_entries() );
		(void)ubi_cacheSetMaxMemory(  mangled_cache, lp_mangled_cache_memory() );
	*/
}

/* ************************************************************************** **
 * Add a mangled name into the cache.
 *
 *  Notes:  If the mangled cache has not been initialized, then the
 *          function will simply fail.  It could initialize the cache,
 *          but that's not the way it was done before I changed the
 *          cache mechanism, so I'm sticking with the old method.
 *
 *          If the extension of the raw name maps directly to the
 *          extension of the mangled name, then we'll store both names
 *          *without* extensions.  That way, we can provide consistent
 *          reverse mangling for all names that match.  The test here is
 *          a bit more careful than the one done in earlier versions of
 *          mangle.c:
 *
 *            - the extension must exist on the raw name,
 *            - it must be all lower case
 *            - it must match the mangled extension (to prove that no
 *              mangling occurred).
 *
 *  crh 07-Apr-1998
 *
 * ************************************************************************** **
 */
static void cache_mangled_name( char *mangled_name, char *raw_name )
{
  ubi_cacheEntryPtr new_entry;
  char             *s1;
  char             *s2;
  size_t               mangled_len;
  size_t               raw_len;
  size_t               i;

  /* If the cache isn't initialized, give up. */
  if( !mc_initialized )
    return;

  /* Init the string lengths. */
  mangled_len = strlen( mangled_name );
  raw_len     = strlen( raw_name );

  /* See if the extensions are unmangled.  If so, store the entry
   * without the extension, thus creating a "group" reverse map.
   */
  s1 = strrchr( mangled_name, '.' );
  if( s1 && (s2 = strrchr( raw_name, '.' )) )
    {
    i = 1;
    while( s1[i] && (tolower( s1[i] ) == s2[i]) )
      i++;
    if( !s1[i] && !s2[i] )
      {
      mangled_len -= i;
      raw_len     -= i;
      }
    }

  /* Allocate a new cache entry.  If the allocation fails, just return. */
  i = sizeof( ubi_cacheEntry ) + mangled_len + raw_len + 2;
  new_entry = malloc( i );
  if( !new_entry )
    return;

  /* Fill the new cache entry, and add it to the cache. */
  s1 = (char *)(new_entry + 1);
  s2 = (char *)&(s1[mangled_len + 1]);
  (void)StrnCpy( s1, mangled_name, mangled_len );
  (void)StrnCpy( s2, raw_name,     raw_len );
  ubi_cachePut( mangled_cache, i, new_entry, s1 );
}

/* ************************************************************************** **
 * Check for a name on the mangled name stack
 *
 *  Input:  s - Input *and* output string buffer.
 *
 *  Output: True if the name was found in the cache, else False.
 *
 *  Notes:  If a reverse map is found, the function will overwrite the string
 *          space indicated by the input pointer <s>.  This is frightening.
 *          It should be rewritten to return NULL if the long name was not
 *          found, and a pointer to the long name if it was found.
 *
 * ************************************************************************** **
 */

static BOOL check_mangled_cache( char *s, size_t maxlen )
{
  ubi_cacheEntryPtr FoundPtr;
  char             *ext_start = NULL;
  char             *found_name;
  char             *saved_ext = NULL;

  /* If the cache isn't initialized, give up. */
  if( !mc_initialized )
    return( False );

  FoundPtr = ubi_cacheGet( mangled_cache, (ubi_trItemPtr)s );

  /* If we didn't find the name *with* the extension, try without. */
  if( !FoundPtr )
  {
    ext_start = strrchr( s, '.' );
    if( ext_start )
    {
      if((saved_ext = strdup(ext_start)) == NULL)
        return False;

      *ext_start = '\0';
      FoundPtr = ubi_cacheGet( mangled_cache, (ubi_trItemPtr)s );
      /* 
       * At this point s is the name without the
       * extension. We re-add the extension if saved_ext
       * is not null, before freeing saved_ext.
       */
    }
  }

  /* Okay, if we haven't found it we're done. */
  if( !FoundPtr )
  {
    if(saved_ext)
    {
      /* Replace the saved_ext as it was truncated. */
      (void)safe_strcat( s, saved_ext, maxlen );
      SAFE_FREE(saved_ext);
    }
    return( False );
  }

  /* If we *did* find it, we need to copy it into the string buffer. */
  found_name = (char *)(FoundPtr + 1);
  found_name += (strlen( found_name ) + 1);

  DEBUG( 3, ("Found %s on mangled stack ", s) );

  (void)safe_strcpy( s, found_name, maxlen );
  if( saved_ext )
  {
    /* Replace the saved_ext as it was truncated. */
    (void)safe_strcat( s, saved_ext,maxlen );
    SAFE_FREE(saved_ext);
  }

  DEBUG( 3, ("as %s\n", s) );

  return( True );
}

/*****************************************************************************
 * do the actual mangling to 8.3 format
 * the buffer must be able to hold 13 characters (including the null)
 *****************************************************************************
 */
static void mangle_name_83( char *s)
{
  int csum;
  char *p;
  char extension[4];
  char base[9];
  int baselen = 0;
  int extlen = 0;
  int skip;

  extension[0] = 0;
  base[0] = 0;

  p = strrchr(s,'.');  
  if( p && (strlen(p+1) < (size_t)4) )
    {
    BOOL all_normal = ( strisnormal(p+1) ); /* XXXXXXXXX */

    if( all_normal && p[1] != 0 )
      {
      *p = 0;
      csum = str_checksum( s );
      *p = '.';
      }
    else
      csum = str_checksum(s);
    }
  else
    csum = str_checksum(s);

  strupper( s );

  DEBUG( 5, ("Mangling name %s to ",s) );

  if( p )
    {
    if( p == s )
      safe_strcpy( extension, "___", 3 );
    else
      {
      *p++ = 0;
      while( *p && extlen < 3 )
        {
        skip = get_character_len( *p );
        switch( skip )
          {
          case 2: 
            if( extlen < 2 )
              {
              extension[extlen++] = p[0];
              extension[extlen++] = p[1];
              }
            else 
              {
              extension[extlen++] = mangle( (unsigned char)*p );
              }
            p += 2;
            break;
          case 1:
            extension[extlen++] = p[0];
            p++;
            break;
          default:
            if( isdoschar (*p) && *p != '.' )
              extension[extlen++] = p[0];
            p++;
            break;
          }
        }
      extension[extlen] = 0;
      }
    }

  p = s;

  while( *p && baselen < 5 )
    {
    skip = get_character_len(*p);
    switch( skip )
      {
      case 2:
        if( baselen < 4 )
          {
          base[baselen++] = p[0];
          base[baselen++] = p[1];
          }
        else 
          {
          base[baselen++] = mangle( (unsigned char)*p );
          }
        p += 2;
        break;
      case 1:
        base[baselen++] = p[0];
        p++;
        break;
      default:
        if( isdoschar( *p ) && *p != '.' )
          base[baselen++] = p[0];
        p++;
        break;
      }
    }
  base[baselen] = 0;

  csum = csum % (MANGLE_BASE*MANGLE_BASE);

  (void)slprintf(s, 12, "%s%c%c%c",
                 base, magic_char, mangle( csum/MANGLE_BASE ), mangle( csum ) );

  if( *extension )
    {
    (void)pstrcat( s, "." );
    (void)pstrcat( s, extension );
    }

  DEBUG( 5, ( "%s\n", s ) );

}

/*****************************************************************************
 * Convert a filename to DOS format.  Return True if successful.
 *
 *  Input:  OutName - Source *and* destination buffer. 
 *
 *                    NOTE that OutName must point to a memory space that
 *                    is at least 13 bytes in size!
 *
 *          need83  - If False, name mangling will be skipped unless the
 *                    name contains illegal characters.  Mapping will still
 *                    be done, if appropriate.  This is probably used to
 *                    signal that a client does not require name mangling,
 *                    thus skipping the name mangling even on shares which
 *                    have name-mangling turned on.
 *          cache83 - If False, the mangled name cache will not be updated.
 *                    This is usually used to prevent that we overwrite
 *                    a conflicting cache entry prematurely, i.e. before
 *                    we know whether the client is really interested in the
 *                    current name.  (See PR#13758).  UKD.
 *          snum    - Share number.  This identifies the share in which the
 *                    name exists.
 *
 *  Output: Returns False only if the name wanted mangling but the share does
 *          not have name mangling turned on.
 *
 * ****************************************************************************
 */
static void name_map_mangle(char *OutName, BOOL need83, BOOL cache83)
{
	DEBUG(5,("name_map_mangle( %s, need83 = %s, cache83 = %s )\n", OutName,
		need83 ? "True" : "False", cache83 ? "True" : "False" ));

#ifdef MANGLE_LONG_FILENAMES
	if( !need83 && is_illegal_name(OutName) )
		need83 = True;
#endif  

	/* check if it's already in 8.3 format */
	if (need83 && !is_8_3(OutName, True, False)) {
		char *tmp = NULL; 

		/* mangle it into 8.3 */
		if (cache83)
			tmp = strdup(OutName);

		mangle_name_83(OutName);

		if(tmp != NULL) {
			cache_mangled_name(OutName, tmp);
			SAFE_FREE(tmp);
		}
	}

	DEBUG(5,("name_map_mangle() ==> [%s]\n", OutName));
}

/*
 *   the following provides the abstraction layer to make it easier
 *     to drop in an alternative mangling implementation
 *     */
static struct mangle_fns mangle_fns = {
	is_mangled,
	is_8_3,
	reset_mangled_cache,
	check_mangled_cache,
	name_map_mangle
};

/* return the methods for this mangling implementation */
struct mangle_fns *mangle_hash_init(void)
{
	reset_mangled_cache();
	return &mangle_fns;
}
