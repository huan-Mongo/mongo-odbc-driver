/*
  Copyright (C) 1995-2007 MySQL AB

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  There are special exceptions to the terms and conditions of the GPL
  as it is applied to this software. View the full text of the exception
  in file LICENSE.exceptions in the top-level directory of this software
  distribution.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/**
  @file  utility.c
  @brief Utility functions
*/

#include "myodbc3.h"
#include "errmsg.h"
#include <ctype.h>

#if MYSQL_VERSION_ID >= 40100
# undef USE_MB
#endif


/**
  Execute a SQL statement.

  @param[in] dbc   The database connection
  @param[in] query The query to execute
*/
SQLRETURN odbc_stmt(DBC FAR *dbc, const char *query)
{
    SQLRETURN result= SQL_SUCCESS;

    MYODBCDbgEnter;
    MYODBCDbgInfo( "stmt: %s", query );

    pthread_mutex_lock(&dbc->lock);
    if ( check_if_server_is_alive(dbc) ||
         mysql_real_query(&dbc->mysql,query,strlen(query)) )
    {
        result= set_conn_error(dbc,MYERR_S1000,mysql_error(&dbc->mysql),
                               mysql_errno(&dbc->mysql));
    }
    pthread_mutex_unlock(&dbc->lock);
    MYODBCDbgReturnReturn(result);
}


/**
  Link a list of fields to the current statement result.

  @todo This is a terrible idea. We need to purge this.

  @param[in] stmt        The statement to modify
  @param[in] fields      The fields to attach to the statement
  @param[in] field_count The number of fields
*/
void mysql_link_fields(STMT *stmt, MYSQL_FIELD *fields, uint field_count)
{
    MYSQL_RES *result;
    pthread_mutex_lock(&stmt->dbc->lock);
    result= stmt->result;
    result->fields= fields;
    result->field_count= field_count;
    result->current_field= 0;
    fix_result_types(stmt);
    pthread_mutex_unlock(&stmt->dbc->lock);
}


/**
  Figure out the ODBC result types for each column in the result set.

  @param[in] stmt The statement with result types to be fixed.
*/
void fix_result_types(STMT *stmt)
{
    uint i;
    MYSQL_RES *result= stmt->result;

    MYODBCDbgEnter;

    stmt->state= ST_EXECUTED;  /* Mark set found */
    if ( (stmt->odbc_types= (SQLSMALLINT*)
          my_malloc(sizeof(SQLSMALLINT)*result->field_count, MYF(0))) )
    {
        for ( i= 0 ; i < result->field_count ; i++ )
        {
            MYSQL_FIELD *field= result->fields+i;
            stmt->odbc_types[i]= (SQLSMALLINT) unireg_to_c_datatype(field);
        }
    }
    /*
      Fix default values for bound columns
      Normally there isn't any bound columns at this stage !
    */
    if ( stmt->bind )
    {
        if ( stmt->bound_columns < result->field_count )
        {
            if ( !(stmt->bind= (BIND*) my_realloc((char*) stmt->bind,
                                                  sizeof(BIND) * result->field_count,
                                                  MYF(MY_FREE_ON_ERROR))) )
            {
                /* We should in principle give an error here */
                stmt->bound_columns= 0;
                MYODBCDbgReturnVoid;
            }
            bzero((gptr) (stmt->bind+stmt->bound_columns),
                  (result->field_count -stmt->bound_columns)*sizeof(BIND));
            stmt->bound_columns= result->field_count;
        }
        /* Fix default types and pointers to fields */

        mysql_field_seek(result,0);
        for ( i= 0; i < result->field_count ; i++ )
        {
            if ( stmt->bind[i].fCType == SQL_C_DEFAULT )
                stmt->bind[i].fCType= stmt->odbc_types[i];
            stmt->bind[i].field= mysql_fetch_field(result);
        }
    }
    MYODBCDbgReturnVoid;
}


/**
  Change a string with a length to a NUL-terminated string.

  @param[in,out] to      A buffer to write the string into, which must be at
                         at least length + 1 bytes long.
  @param[in]     from    A pointer to the beginning of the source string.
  @param[in]     length  The length of the string, or SQL_NTS if it is
                         already NUL-terminated.

  @return A pointer to a NUL-terminated string.
*/
char *fix_str(char *to, const char *from, int length)
{
    if ( !from )
        return "";
    if ( length == SQL_NTS )
        return (char *)from;
    strmake(to,from,length);
    return to;
}


/*
  @type    : myodbc internal
  @purpose : duplicate the string
*/

char *dupp_str(char *from,int length)
{
    char *to;
    if ( !from )
        return my_strdup("",MYF(MY_WME));
    if ( length == SQL_NTS )
        length= strlen(from);
    if ( (to= my_malloc(length+1,MYF(MY_WME))) )
    {
        memcpy(to,from,length);
        to[length]= 0;
    }
    return to;
}


/*
  @type    : myodbc internal
  @purpose : copies the string data to rgbValue buffer. If rgbValue
  is NULL, then returns warning with full length, else
  copies the cbValueMax length from 'src' and returns it.
*/

SQLRETURN copy_str_data(SQLSMALLINT HandleType, SQLHANDLE Handle,
                        SQLCHAR FAR *rgbValue,
                        SQLSMALLINT cbValueMax,
                        SQLSMALLINT FAR *pcbValue,char FAR *src)
{
    SQLSMALLINT dummy;

    if ( !pcbValue )
        pcbValue= &dummy;

    if ( cbValueMax == SQL_NTS )
        cbValueMax= *pcbValue= strlen(src);

    else if ( cbValueMax < 0 )
        return set_handle_error(HandleType,Handle,MYERR_S1090,NULL,0);
    else
    {
        cbValueMax= cbValueMax ? cbValueMax - 1 : 0;
        *pcbValue= strlen(src);
    }

    if ( rgbValue )
        strmake((char*) rgbValue, src, cbValueMax);

    if ( min(*pcbValue , cbValueMax) != *pcbValue )
        return SQL_SUCCESS_WITH_INFO;
    return SQL_SUCCESS;
}


/*
  @type    : myodbc internal
  @purpose : returns (possibly truncated) results
  if result is truncated the result length contains
  length of the truncted result
*/

SQLRETURN
copy_lresult(SQLSMALLINT HandleType, SQLHANDLE Handle,
             SQLCHAR FAR *rgbValue, SQLINTEGER cbValueMax,
             SQLLEN *pcbValue,char *src,long src_length,
             long max_length,long fill_length,ulong *offset,
             my_bool binary_data)
{
    char *dst= (char*) rgbValue;
    ulong length;
    SQLINTEGER arg_length;

    if ( src && src_length == SQL_NTS )
        src_length= strlen(src);

    arg_length= cbValueMax;
    if ( cbValueMax && !binary_data )   /* If not length check */
        cbValueMax--;   /* Room for end null */
    else if ( !cbValueMax )
        dst= 0;     /* Don't copy anything! */
    if ( max_length )   /* If limit on char lengths */
    {
        set_if_smaller(cbValueMax,(long) max_length);
        set_if_smaller(src_length,max_length);
        set_if_smaller(fill_length,max_length);
    }
    if ( HandleType == SQL_HANDLE_DBC )
    {
        if ( fill_length < src_length || !Handle ||
             !(((DBC FAR*)Handle)->flag & FLAG_PAD_SPACE) )
            fill_length= src_length;
    }
    else
    {
        if ( fill_length < src_length || !Handle ||
             !(((STMT FAR*)Handle)->dbc->flag & FLAG_PAD_SPACE) )
            fill_length= src_length;
    }
    if ( *offset == (ulong) ~0L )
        *offset= 0;         /* First call */
    else if ( arg_length && *offset >= (ulong) fill_length )
        return SQL_NO_DATA_FOUND;

    src+= *offset;
    src_length-= (long) *offset;
    fill_length-= *offset;

    length= min(fill_length, cbValueMax);
    (*offset)+= length;        /* Fix for next call */
    if ( pcbValue )
        *pcbValue= fill_length;
    if ( dst )      /* Bind allows null pointers */
    {
        ulong copy_length= ((long) src_length >= (long) length ? length :
                            ((long) src_length >= 0 ? src_length : 0L));
        memcpy(dst,src,copy_length);
        bfill(dst+copy_length,length-copy_length,' ');
        if ( !binary_data || length != (ulong) cbValueMax )
            dst[length]= 0;
    }
    if ( arg_length && cbValueMax >= fill_length )
        return SQL_SUCCESS;
    MYODBCDbgInfo( "Returned %ld characters from", length );
    MYODBCDbgInfo( "offset: %lu", *offset - length );
    set_handle_error(HandleType,Handle,MYERR_01004,NULL,0);
    return SQL_SUCCESS_WITH_INFO;
}


/**
  Copy a string from one character set to another. Taken from sql_string.cc
  in the MySQL Server source code, since we don't export this functionality
  in libmysql yet.

  @c to must be at least as big as @c from_length * @c to_cs->mbmaxlen

  @param[in,out] to           Store result here
  @param[in]     to_cs        Character set of result string
  @param[in]     from         Copy from here
  @param[in]     from_length  Length of from string
  @param[in]     from_cs      From character set
  @param[in,out] errors       Number of errors encountered during conversion

  @retval Length of bytes copied to @c to
*/
uint32
copy_and_convert(char *to, uint32 to_length, CHARSET_INFO *to_cs,
                 const char *from, uint32 from_length, CHARSET_INFO *from_cs,
                 uint *errors)
{
  int         cnvres;
  my_wc_t     wc;
  const uchar *from_end= (const uchar*) from+from_length;
  char *to_start= to;
  uchar *to_end= (uchar*) to+to_length;
  int (*mb_wc)(struct charset_info_st *, my_wc_t *, const uchar *,
               const uchar *) = from_cs->cset->mb_wc;
  int (*wc_mb)(struct charset_info_st *, my_wc_t, uchar *s, uchar *e)=
    to_cs->cset->wc_mb;
  uint error_count= 0;

  while (1)
  {
    if ((cnvres= (*mb_wc)(from_cs, &wc, (uchar*) from, from_end)) > 0)
      from+= cnvres;
    else if (cnvres == MY_CS_ILSEQ)
    {
      error_count++;
      from++;
      wc= '?';
    }
    else if (cnvres > MY_CS_TOOSMALL)
    {
      /*
        A correct multibyte sequence detected
        But it doesn't have Unicode mapping.
      */
      error_count++;
      from+= (-cnvres);
      wc= '?';
    }
    else
      break;  // Not enough characters

outp:
    if ((cnvres= (*wc_mb)(to_cs, wc, (uchar*) to, to_end)) > 0)
      to+= cnvres;
    else if (cnvres == MY_CS_ILUNI && wc != '?')
    {
      error_count++;
      wc= '?';
      goto outp;
    }
    else
      break;
  }
  *errors= error_count;
  return (uint32) (to - to_start);
}


/**
  Copy a result from the server into a buffer as a SQL_C_WCHAR.

  @param[in]     HandleType  Type of handle (@c SQL_HANDLE_DBC or
                             @c SQL_HANDLE_STMT)
  @param[in]     Handle      Handle
  @param[out]    result      Buffer for result
  @param[in]     result_len  Size of result buffer
  @param[out]    used_len    Pointer to buffer for storing amount of buffer used
  @param[in]     src         Source data for result
  @param[in]     src_len     Length of source data (in bytes)
  @param[in]     max_len     Maximum length (SQL_ATTR_MAX_LENGTH)
  @param[in]     fill_len    The display length, in bytes, of the source data
  @param[in,out] offset      Offset into source to copy from (will be updated)

  @return Standard ODBC result code
*/
SQLRETURN
copy_wchar_result(SQLSMALLINT HandleType, SQLHANDLE Handle,
                  SQLWCHAR *result, SQLINTEGER result_len, SQLLEN *used_len,
                  char *src, long src_len, long max_len, long fill_len,
                  ulong *offset)
{
  CHARSET_INFO *charset;
  SQLWCHAR *dst= result;
  SQLINTEGER orig_result_len= result_len;
  ulong length;
  my_bool pad_space= FALSE;

  /* Calculate actual source length if we got SQL_NTS */
  if (src_len == SQL_NTS)
    src_len= src ? strlen(src) : 0;

  if (result_len)
    result_len--; /* Need room for end nul */
  else
    dst= 0; /* Don't copy anything! */

  /* Apply max length, if one was specified. */
  if (max_len && max_len < result_len)
    result_len= max_len;

  /* Get the character set and whether FLAG_PAD_SPACE is set.  */
  if (HandleType == SQL_HANDLE_DBC)
  {
    charset= ((DBC *)Handle)->mysql.charset;
    pad_space= ((DBC *)Handle)->flag & FLAG_PAD_SPACE;
  }
  else
  {
    charset= ((STMT *)Handle)->dbc->mysql.charset;
    pad_space= ((STMT *)Handle)->dbc->flag & FLAG_PAD_SPACE;
  }

  if (fill_len < src_len || !pad_space)
    fill_len= src_len;

  if (*offset == (ulong) ~0L)
    *offset= 0;         /* First call */
  else if (orig_result_len && *offset >= (ulong) fill_len)
    return SQL_NO_DATA_FOUND;

  /* Skip already-retrieved data. */
  src+= *offset;
  src_len-= (long) *offset;
  fill_len-= *offset;

  /* Figure out how many characters we actually have left to copy into.  */
  length= min(fill_len, result_len);

  if (dst)
  {
    ulong i, bytes, copy_len= (src_len >= (long)length ? length :
                               (src_len > 0 ? src_len : 0L));
    UTF8 *temp= (UTF8 *)my_malloc(copy_len * 4, MYF(0));
    uint errors;

    bytes= copy_and_convert((char *)temp, copy_len * 4, utf8_charset_info,
                             src, copy_len, charset, &errors);

    /* Update offset for the next call. */
    (*offset)+= bytes;

    if (sizeof(SQLWCHAR) == 4)
    {
      for (i= 0; i < bytes; )
        i+= utf8toutf32(temp + i, (UTF32 *)dst++);
    }
    else
    {
      for (i= 0; i < bytes; )
      {
        UTF32 u32;
        i+= utf8toutf32(temp + i, &u32);
        dst+= utf32toutf16(u32, (UTF16 *)dst);
      }
    }

    while (dst < result + length)
      *dst++= ' ';

    if (length != (ulong) result_len)
      dst= 0;
  }

  if (used_len)
    *used_len= fill_len;

  if (orig_result_len && result_len >= fill_len)
    return SQL_SUCCESS;

  MYODBCDbgInfo("Returned %ld characters from", length);
  MYODBCDbgInfo("offset: %lu", *offset - length);

  set_handle_error(HandleType, Handle, MYERR_01004, NULL, 0);

  return SQL_SUCCESS_WITH_INFO;
}


/*
  @type    : myodbc internal
  @purpose : is used when converting a binary string to a SQL_C_CHAR
*/

SQLRETURN copy_binary_result( SQLSMALLINT   HandleType, 
                              SQLHANDLE     Handle,
                              SQLCHAR FAR * rgbValue,
                              SQLINTEGER    cbValueMax,
                              SQLLEN *      pcbValue,
                              char *        src,
                              ulong         src_length,
                              ulong         max_length,
                              ulong *       offset )
{
    char *dst= (char*) rgbValue;
    ulong length;
#if MYSQL_VERSION_ID >= 40100
    char NEAR _dig_vec[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
#endif

    if ( !cbValueMax )
        dst= 0;  /* Don't copy anything! */
    if ( max_length ) /* If limit on char lengths */
    {
        set_if_smaller(cbValueMax,(long) max_length+1);
        set_if_smaller(src_length,(max_length+1)/2);
    }
    if ( *offset == (ulong) ~0L )
        *offset= 0;   /* First call */
    else if ( *offset >= src_length )
        return SQL_NO_DATA_FOUND;
    src+= *offset;
    src_length-= *offset;
    length= cbValueMax ? (ulong)(cbValueMax-1)/2 : 0;
    length= min(src_length,length);
    (*offset)+= length;     /* Fix for next call */
    if ( pcbValue )
        *pcbValue= src_length*2;
    if ( dst )  /* Bind allows null pointers */
    {
        ulong i;
        for ( i= 0 ; i < length ; i++ )
        {
            *dst++= _dig_vec[(uchar) *src >> 4];
            *dst++= _dig_vec[(uchar) *src++ & 15];
        }
        *dst= 0;
    }
    if ( (ulong) cbValueMax > length*2 )
        return SQL_SUCCESS;
    MYODBCDbgInfo( "Returned %ld characters from", length );
    MYODBCDbgInfo( "offset: %ld", *offset - length );

    set_handle_error(HandleType,Handle,MYERR_01004,NULL,0);
    return SQL_SUCCESS_WITH_INFO;
}


/*
  @type    : myodbc internal
  @purpose : get type, transfer length and precision for a unireg column
  note that timestamp is changed to YYYY-MM-DD HH:MM:SS type

  SQLUINTEGER

*/

int unireg_to_sql_datatype(STMT FAR *stmt, MYSQL_FIELD *field, char *buff,
                           ulong *transfer_length, ulong *precision,
                           ulong *display_size)
{
    char *pos;
    my_bool field_is_binary= binary_field(field);
/* PAH - SESSION 01
    if ( stmt->dbc->flag & (FLAG_FIELD_LENGTH | FLAG_SAFE) )
*/        *transfer_length= *precision= *display_size= max(field->length,
                                                         field->max_length);
/* PAH - SESSION 01
    else

        *transfer_length= *precision= *display_size= field->max_length;
*/

/* PAH - SESSION 01
printf( "[PAH][%s][%d][%s] field->type=%d field_is_binary=%d\n", __FILE__, __LINE__, __FUNCTION__, field->type, field_is_binary );
*/
    switch ( field->type )
    {
        case MYSQL_TYPE_BIT:
            if ( buff )
            {
                pos= strmov(buff,"bit");
            }
            *transfer_length= 1;
            return SQL_BIT;  

        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
            *display_size= max(field->length,field->max_length) -
                           test(!(field->flags & UNSIGNED_FLAG)) -
                           test(field->decimals);
            *precision= *display_size;
            if ( buff ) strmov(buff,"decimal");
            return SQL_DECIMAL;

        case MYSQL_TYPE_TINY:
            if ( num_field(field) )
            {
                if ( buff )
                {
                    pos= strmov(buff,"tinyint");
                    if ( field->flags & UNSIGNED_FLAG )
                        strmov(pos," unsigned");
                }
                *transfer_length= 1;
                return SQL_TINYINT;
            }
            if ( buff )
            {
                pos= strmov(buff,"char");
                if ( field->flags & UNSIGNED_FLAG )
                    strmov(pos," unsigned");
            }
            *transfer_length= 1;
            return SQL_CHAR;

        case MYSQL_TYPE_SHORT:
            if ( buff )
            {
                pos= strmov(buff,"smallint");
                if ( field->flags & UNSIGNED_FLAG )
                    strmov(pos," unsigned");
            }
            *transfer_length= 2;
            return SQL_SMALLINT;

        case MYSQL_TYPE_INT24:
            if ( buff )
            {
                pos= strmov(buff,"mediumint");
                if ( field->flags & UNSIGNED_FLAG )
                    strmov(pos," unsigned");
            }
            *transfer_length= 4;
            return SQL_INTEGER;

        case MYSQL_TYPE_LONG:
            if ( buff )
            {
                pos= strmov(buff,"integer");
                if ( field->flags & UNSIGNED_FLAG )
                    strmov(pos," unsigned");
            }
            *transfer_length= 4;
            return SQL_INTEGER;

        case MYSQL_TYPE_LONGLONG:
            if ( buff )
            {
                pos= strmov(buff,"bigint");
                if ( field->flags & UNSIGNED_FLAG )
                    strmov(pos," unsigned");
            }
            *transfer_length= 20;
            if ( stmt->dbc->flag & FLAG_NO_BIGINT )
                return SQL_INTEGER;
            if ( field->flags & UNSIGNED_FLAG )
                *transfer_length= *precision= 20;
            else
                *transfer_length= *precision= 19;
            return SQL_BIGINT;

        case MYSQL_TYPE_FLOAT:
            if ( buff )
            {
                pos= strmov(buff,"float");
                if ( field->flags & UNSIGNED_FLAG )
                    strmov(pos," unsigned");
            }
            *transfer_length= 4;
            return SQL_REAL;
        case MYSQL_TYPE_DOUBLE:
            if ( buff )
            {
                pos= strmov(buff,"double");
                if ( field->flags & UNSIGNED_FLAG )
                    strmov(pos," unsigned");
            }
            *transfer_length= 8;
            return SQL_DOUBLE;

        case MYSQL_TYPE_NULL:
            if ( buff ) strmov(buff,"null");
            return SQL_VARCHAR;

        case MYSQL_TYPE_YEAR:
            if ( buff )
                pos= strmov(buff,"year");
            *transfer_length= 2;
            return SQL_SMALLINT;

        case MYSQL_TYPE_TIMESTAMP:
            if ( buff ) strmov(buff,"timestamp");
            *transfer_length= 16;      /* size of timestamp_struct */
            *precision= *display_size= 19;
            if ( stmt->dbc->env->odbc_ver == SQL_OV_ODBC3 )
                return SQL_TYPE_TIMESTAMP;
            return SQL_TIMESTAMP;

        case MYSQL_TYPE_DATETIME:
            if ( buff ) strmov(buff,"datetime");
            *transfer_length= 16;      /* size of timestamp_struct */
            *precision= *display_size= 19;
            if ( stmt->dbc->env->odbc_ver == SQL_OV_ODBC3 )
                return SQL_TYPE_TIMESTAMP;
            return SQL_TIMESTAMP;

        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_DATE:
            if ( buff ) strmov(buff,"date");
            *transfer_length= 6;       /* size of date struct */
            *precision= *display_size= 10;
            if ( stmt->dbc->env->odbc_ver == SQL_OV_ODBC3 )
                return SQL_TYPE_DATE;
            return SQL_DATE;

        case MYSQL_TYPE_TIME:
            if ( buff ) strmov(buff,"time");
            *transfer_length= 6;       /* size of time struct */
            *precision= *display_size= 8;
            if ( stmt->dbc->env->odbc_ver == SQL_OV_ODBC3 )
                return SQL_TYPE_TIME;
            return SQL_TIME;

        case MYSQL_TYPE_STRING:
            /* Binary flag is for handling "VARCHAR() BINARY" but is unreliable (see BUG-4578) - PAH */
            if (field_is_binary)
            {
              if (buff) strmov(buff,"binary");
              return SQL_BINARY;
            }

            *transfer_length= *precision= *display_size= field->length ? 
                (stmt->dbc->mysql.charset ? 
                field->length/stmt->dbc->mysql.charset->mbmaxlen: field->length): 255;
            if ( buff ) strmov(buff,"char");
            return SQL_CHAR;

        /*
          MYSQL_TYPE_VARCHAR is never actually sent, this just silences
          a compiler warning.
        */
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
            /* 
            TODO: field->length should be replaced by max(length, maxlength)
                  in order to restore FLAG_FIELD_LENGTH option
            */
            *transfer_length= *precision= *display_size= field->length ?
              (stmt->dbc->mysql.charset ?
               field->length / stmt->dbc->mysql.charset->mbmaxlen :
               field->length) : 255;

            /*
            TODO: Uncomment this code when MySQL Server returns the metadata correctly

            if (field_is_binary)
            {
                if (buff)
                  strmov(buff,"varbinary");
                return SQL_VARBINARY;
            }
            */

            if (buff)
              strmov(buff,"varchar");

            return SQL_VARCHAR;

        case MYSQL_TYPE_TINY_BLOB:
            if ( buff )
                strmov(buff,(field_is_binary) ? "tinyblob" : "tinytext");
/* PAH - SESSION 01
            if ( stmt->dbc->flag & (FLAG_FIELD_LENGTH | FLAG_SAFE) )
*/
                *transfer_length= *precision= *display_size= field->length ? 
                (stmt->dbc->mysql.charset ?
                field->length/stmt->dbc->mysql.charset->mbmaxlen: field->length): 255;
            return(field_is_binary) ? SQL_LONGVARBINARY : SQL_LONGVARCHAR;

        case MYSQL_TYPE_BLOB:
            if ( buff )
                strmov( buff, (field_is_binary) ? "blob" : "text" );

/* PAH - SESSION 01
            if ( stmt->dbc->flag & (FLAG_FIELD_LENGTH | FLAG_SAFE) ) 
*/
                *transfer_length= *precision= *display_size= field->length ? (stmt->dbc->mysql.charset ? field->length/stmt->dbc->mysql.charset->mbmaxlen: field->length): 65535;

            return ( field_is_binary ) ? SQL_LONGVARBINARY : SQL_LONGVARCHAR;

        case MYSQL_TYPE_MEDIUM_BLOB:
            if ( buff )
                strmov(buff,((field_is_binary) ? "mediumblob" :
                             "mediumtext"));
/* PAH - SESSION 01
            if ( stmt->dbc->flag & (FLAG_FIELD_LENGTH | FLAG_SAFE) )
*/
                *transfer_length= *precision= *display_size= field->length ? 
                (stmt->dbc->mysql.charset ?
                field->length/stmt->dbc->mysql.charset->mbmaxlen: field->length): (1L << 24)-1L;
            return(field_is_binary) ? SQL_LONGVARBINARY : SQL_LONGVARCHAR;

        case MYSQL_TYPE_LONG_BLOB:
            if ( buff )
                strmov(buff,((field_is_binary) ? "longblob": "longtext"));
/* PAH - SESSION 01
            if ( stmt->dbc->flag & (FLAG_FIELD_LENGTH | FLAG_SAFE) )
*/
                *transfer_length= *precision= *display_size= field->length ? 
                (stmt->dbc->mysql.charset ?
                field->length/stmt->dbc->mysql.charset->mbmaxlen: field->length): INT_MAX32;
            return(field_is_binary) ? SQL_LONGVARBINARY : SQL_LONGVARCHAR;

        case MYSQL_TYPE_ENUM:
            if ( buff )
                strmov(buff,"enum");
            return SQL_CHAR;

        case MYSQL_TYPE_SET:
            if ( buff )
                strmov(buff,"set");
            return SQL_CHAR;

        case MYSQL_TYPE_GEOMETRY:
            if ( buff )
                strmov(buff,"blob");
            return SQL_LONGVARBINARY;
    }
    return 0; /* Impossible */
}


/*
  @type    : myodbc internal
  @purpose : returns internal type to C type
*/

int unireg_to_c_datatype(MYSQL_FIELD *field)
{
    switch ( field->type )
    {
        case MYSQL_TYPE_LONGLONG: /* Must be returned as char */
        default:
            return SQL_C_CHAR;
        case MYSQL_TYPE_BIT:
            return SQL_C_BIT;
        case MYSQL_TYPE_TINY:
            return SQL_C_TINYINT;
        case MYSQL_TYPE_YEAR:
        case MYSQL_TYPE_SHORT:
            return SQL_C_SHORT;
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
            return SQL_C_LONG;
        case MYSQL_TYPE_FLOAT:
            return SQL_C_FLOAT;
        case MYSQL_TYPE_DOUBLE:
            return SQL_C_DOUBLE;
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_DATETIME:
            return SQL_C_TIMESTAMP;
        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_DATE:
            return SQL_C_DATE;
        case MYSQL_TYPE_TIME:
            return SQL_C_TIME;
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
            return SQL_C_BINARY;
    }
}


/*
  @type    : myodbc internal
  @purpose : returns default C type for a given SQL type
*/

int default_c_type(int sql_data_type)
{
    switch ( sql_data_type )
    {
        case SQL_CHAR:
        case SQL_VARCHAR:
        case SQL_LONGVARCHAR:
        case SQL_DECIMAL:
        case SQL_NUMERIC:
        default:
            return SQL_C_CHAR;
        case SQL_BIGINT:
            return SQL_C_SBIGINT;
        case SQL_BIT:
            return SQL_C_BIT;
        case SQL_TINYINT:
            return SQL_C_TINYINT;
        case SQL_SMALLINT:
            return SQL_C_SHORT;
        case SQL_INTEGER:
            return SQL_C_LONG;
        case SQL_REAL:
        case SQL_FLOAT:
            return SQL_C_FLOAT;
        case SQL_DOUBLE:
            return SQL_C_DOUBLE;
        case SQL_BINARY:
        case SQL_VARBINARY:
        case SQL_LONGVARBINARY:
            return SQL_C_BINARY;
        case SQL_DATE:
        case SQL_TYPE_DATE:
            return SQL_C_DATE;
        case SQL_TIME:
        case SQL_TYPE_TIME:
            return SQL_C_TIME;
        case SQL_TIMESTAMP:
        case SQL_TYPE_TIMESTAMP:
            return SQL_C_TIMESTAMP;
    }
}


/*
  @type    : myodbc internal
  @purpose : returns bind length
*/

ulong bind_length(int sql_data_type,ulong length)
{
    switch ( sql_data_type )
    {
        default:                  /* For CHAR, VARCHAR, BLOB...*/
            return length;
        case SQL_C_BIT:
        case SQL_C_TINYINT:
        case SQL_C_STINYINT:
        case SQL_C_UTINYINT:
            return 1;
        case SQL_C_SHORT:
        case SQL_C_SSHORT:
        case SQL_C_USHORT:
            return 2;
        case SQL_C_LONG:
        case SQL_C_SLONG:
        case SQL_C_ULONG:
            return sizeof(SQLINTEGER);
        case SQL_C_FLOAT:
            return sizeof(float);
        case SQL_C_DOUBLE:
            return sizeof(double);
        case SQL_C_DATE:
        case SQL_C_TYPE_DATE:
            return sizeof(DATE_STRUCT);
        case SQL_C_TIME:
        case SQL_C_TYPE_TIME:
            return sizeof(TIME_STRUCT);
        case SQL_C_TIMESTAMP:
        case SQL_C_TYPE_TIMESTAMP:
            return sizeof(TIMESTAMP_STRUCT);
        case SQL_C_SBIGINT:
        case SQL_C_UBIGINT:
            return sizeof(longlong);
    }
}

/*
  @type    : myodbc internal
  @purpose : convert a possible string to a timestamp value
*/

my_bool str_to_ts(SQL_TIMESTAMP_STRUCT *ts, const char *str, int zeroToMin)
{ 
    uint year, length;
    char buff[15],*to;
    SQL_TIMESTAMP_STRUCT tmp_timestamp;

    if ( !ts )
        ts= (SQL_TIMESTAMP_STRUCT *) &tmp_timestamp;

    for ( to= buff ; *str && to < buff+sizeof(buff)-1 ; str++ )
    {
        if ( isdigit(*str) )
            *to++= *str;
    }

    length= (uint) (to-buff);

    if ( length == 6 || length == 12 )  /* YYMMDD or YYMMDDHHMMSS */
    {
        bmove_upp(to+2,to,length);
        if ( buff[0] <= '6' )
        {
            buff[0]='2';
            buff[1]='0';
        }
        else
        {
            buff[0]='1';
            buff[1]='9';
        }
        length+= 2;
        to+= 2;
    }

    if ( length < 14 )
        strfill(to,14 - length,'0');
    else
        *to= 0;

    year= (digit(buff[0])*1000+digit(buff[1])*100+digit(buff[2])*10+digit(buff[3]));

    if (!strncmp(&buff[4], "00", 2) || !strncmp(&buff[6], "00", 2))
    {
      if (!zeroToMin) /* Don't convert invalid */
        return 1;

      /* convert invalid to min allowed */
      if (!strncmp(&buff[4], "00", 2))
        buff[5]= '1';
      if (!strncmp(&buff[6], "00", 2))
        buff[7]= '1';
    }

    ts->year=   year;
    ts->month=  digit(buff[4])*10+digit(buff[5]);
    ts->day=    digit(buff[6])*10+digit(buff[7]);
    ts->hour=   digit(buff[8])*10+digit(buff[9]);
    ts->minute= digit(buff[10])*10+digit(buff[11]);
    ts->second= digit(buff[12])*10+digit(buff[13]);
    ts->fraction= 0;
    return 0;
}

/*
  @type    : myodbc internal
  @purpose : convert a possible string to a time value
*/

my_bool str_to_time_st(SQL_TIME_STRUCT *ts, const char *str)
{ 
    char buff[12],*to;
    SQL_TIME_STRUCT tmp_time;

    if ( !ts )
        ts= (SQL_TIME_STRUCT *) &tmp_time;

    for ( to= buff ; *str && to < buff+sizeof(buff)-1 ; str++ )
    {
        if ( isdigit(*str) )
            *to++= *str;
    }

    ts->hour=   digit(buff[0])*10+digit(buff[1]);
    ts->minute= digit(buff[2])*10+digit(buff[3]);
    ts->second= digit(buff[4])*10+digit(buff[5]);
    return 0;
}

/*
  @type    : myodbc internal
  @purpose : convert a possible string to a data value. if
             zeroToMin is specified, YEAR-00-00 dates will be
             converted to the min valid ODBC date
*/

my_bool str_to_date(SQL_DATE_STRUCT *rgbValue, const char *str,
                    uint length, int zeroToMin)
{
    uint field_length,year_length,digits,i,date[3];
    const char *pos;
    const char *end= str+length;
    for ( ; !isdigit(*str) && str != end ; str++ ) ;
    /*
      Calculate first number of digits.
      If length= 4, 8 or >= 14 then year is of format YYYY
      (YYYY-MM-DD,  YYYYMMDD)
    */
    for ( pos= str; pos != end && isdigit(*pos) ; pos++ ) ;
    digits= (uint) (pos-str);
    year_length= (digits == 4 || digits == 8 || digits >= 14) ? 4 : 2;
    field_length= year_length-1;

    for ( i= 0 ; i < 3 && str != end; i++ )
    {
        uint tmp_value= (uint) (uchar) (*str++ - '0');
        while ( str != end && isdigit(str[0]) && field_length-- )
        {
            tmp_value= tmp_value*10 + (uint) (uchar) (*str - '0');
            str++;
        }
        date[i]= tmp_value;
        while ( str != end && !isdigit(*str) )
            str++;
        field_length= 1;   /* Rest fields can only be 2 */
    }
    if (i <= 1 || (i > 1 && !date[1]) || (i > 2 && !date[2]))
    {
      if (!zeroToMin) /* Convert? */
        return 1;

      rgbValue->year=  date[0];
      rgbValue->month= (i > 1 && date[1]) ? date[1] : 1;
      rgbValue->day=   (i > 2 && date[2]) ? date[2] : 1;
    }
    else
    {
      while ( i < 3 )
        date[i++]= 1;

      rgbValue->year=  date[0];
      rgbValue->month= date[1];
      rgbValue->day=   date[2];
    }
    return 0;
}


/*
  @type    : myodbc internal
  @purpose : convert a time string to a (ulong) value.
  At least following formats are recogniced
  HHMMSS HHMM HH HH.MM.SS  {t HH:MM:SS }
  @return  : HHMMSS
*/

ulong str_to_time_as_long(const char *str,uint length)
{
    uint i,date[3];
    const char *end= str+length;

    if ( length == 0 )
        return 0;

    for ( ; !isdigit(*str) && str != end ; str++ ) length--;

    for ( i= 0 ; i < 3 && str != end; i++ )
    {
        uint tmp_value= (uint) (uchar) (*str++ - '0');
        length--;

        while ( str != end && isdigit(str[0]) )
        {
            tmp_value= tmp_value*10 + (uint) (uchar) (*str - '0');
            str++; 
            length--;
        }
        date[i]= tmp_value;
        while ( str != end && !isdigit(*str) )
        {
            str++;
            length--;
        }
    }
    if ( length && str != end )
        return str_to_time_as_long(str, length);/* timestamp format */

    if ( date[0] > 10000L || i < 3 )    /* Properly handle HHMMSS format */
        return(ulong) date[0];

    return(ulong) date[0] * 10000L + (ulong) (date[1]*100L+date[2]);
}


/*
  @type    : myodbc internal
  @purpose : if there was a long time since last question, check that
  the server is up with mysql_ping (to force a reconnect)
*/

int check_if_server_is_alive( DBC FAR *dbc )
{
    time_t seconds= (time_t) time( (time_t*)0 );
    int result= 0;

    if ( (ulong)(seconds - dbc->last_query_time) >= CHECK_IF_ALIVE )
    {
        if ( mysql_ping( &dbc->mysql ) )
        {
            /*  BUG: 14639

                A. The 4.1 doc says when mysql_ping() fails we can get one
                of the following errors from mysql_errno();

                    CR_COMMANDS_OUT_OF_SYNC
                    CR_SERVER_GONE_ERROR
                    CR_UNKNOWN_ERROR   

                But if you do a mysql_ping() after bringing down the server
                you get CR_SERVER_LOST.

                PAH - 9.MAR.06
            */
            
            if ( mysql_errno( &dbc->mysql ) == CR_SERVER_LOST )
                result = 1;
        }
    }
    dbc->last_query_time = seconds;

    return result;
}


/*
  @type    : myodbc3 internal
  @purpose : appends quoted string to dynamic string
*/

my_bool dynstr_append_quoted_name(DYNAMIC_STRING *str, const char *name)
{
    uint tmp= strlen(name);
    char *pos;
    if ( dynstr_realloc(str,tmp+3) )
        return 1;
    pos= str->str+str->length;
    *pos='`';
    memcpy(pos+1,name,tmp);
    pos[tmp+1]='`';
    pos[tmp+2]= 0;        /* Safety */
    str->length+= tmp+2;
    return 0;
}


/*
  @type    : myodbc3 internal
  @purpose : reset the db name to current_database()
*/

my_bool reget_current_catalog(DBC FAR *dbc)
{
    my_free((gptr) dbc->database,MYF(0));
    if ( odbc_stmt(dbc, "select database()") )
    {
        return 1;
    }
    else
    {
        MYSQL_RES *res;
        MYSQL_ROW row;

        if ( (res= mysql_store_result(&dbc->mysql)) &&
             (row= mysql_fetch_row(res)) )
        {
/*            if (cmp_database(row[0], dbc->database)) */
            {
                if ( row[0] )
                    dbc->database = my_strdup(row[0], MYF(MY_WME));
                else
                    dbc->database = strdup( "null" );
            }
        }
        mysql_free_result(res);
    }

    return 0;
}


/*
  @type    : myodbc internal
  @purpose : compare strings without regarding to case
*/

int myodbc_strcasecmp(const char *s, const char *t)
{
#ifdef USE_MB
    if ( use_mb(default_charset_info) )
    {
        register uint32 l;
        register const char *end= s+strlen(s);
        while ( s<end )
        {
            if ( (l= my_ismbchar(default_charset_info, s,end)) )
            {
                while ( l-- )
                    if ( *s++ != *t++ ) return 1;
            }
            else if ( my_ismbhead(default_charset_info, *t) ) return 1;
            else if ( toupper((uchar) *s++) != toupper((uchar) *t++) ) return 1;
        }
        return *t;
    }
    else
#endif
    {
        while ( toupper((uchar) *s) == toupper((uchar) *t++) )
            if ( !*s++ ) return 0;
        return((int) toupper((uchar) s[0]) - (int) toupper((uchar) t[-1]));
    }
}


/*
  @type    : myodbc internal
  @purpose : compare strings without regarding to case
*/

int myodbc_casecmp(const char *s, const char *t, uint len)
{
#ifdef USE_MB
    if ( use_mb(default_charset_info) )
    {
        register uint32 l;
        register const char *end= s+len;
        while ( s<end )
        {
            if ( (l= my_ismbchar(default_charset_info, s,end)) )
            {
                while ( l-- )
                    if ( *s++ != *t++ ) return 1;
            }
            else if ( my_ismbhead(default_charset_info, *t) ) return 1;
            else if ( toupper((uchar) *s++) != toupper((uchar) *t++) ) return 1;
        }
        return 0;
    }
    else
#endif /* USE_MB */
    {
        while ( len-- != 0 && toupper(*s++) == toupper(*t++) ) ;
        return(int) len+1;
    }
}


/*
  @type    : myodbc3 internal
  @purpose : logs the queries sent to server
*/

#ifdef MYODBC_DBG
void query_print(FILE *log_file,char *query)
{
    if ( log_file && query )
        fprintf(log_file, "%s;\n",query);
}


FILE *init_query_log(void)
{
    FILE *query_log;

    if ( (query_log= fopen(DRIVER_QUERY_LOGFILE, "w")) )
    {
        fprintf(query_log,"-- Query logging\n");
        fprintf(query_log,"--\n");
        fprintf(query_log,"--  Driver name: %s  Version: %s\n",DRIVER_NAME,
                DRIVER_VERSION);
#ifdef HAVE_LOCALTIME_R
        {
            time_t now= time(NULL);
            struct tm start;
            localtime_r(&now,&start);

            fprintf(query_log,"-- Timestamp: %02d%02d%02d %2d:%02d:%02d\n",
                    start.tm_year % 100,
                    start.tm_mon+1,
                    start.tm_mday,
                    start.tm_hour,
                    start.tm_min,
                    start.tm_sec);
#endif /* HAVE_LOCALTIME_R */
            fprintf(query_log,"\n");
        }
    }
    return query_log;
}


void end_query_log(FILE *query_log)
{
    if ( query_log )
    {
        fclose(query_log);
        query_log= 0;
    }
}

#else
void query_print(char *query __attribute__((unused)))
{
}
#endif /* !DBUG_OFF */


my_bool is_minimum_version(const char *server_version,const char *version,
                           uint length)
{
    if ( strncmp(server_version,version,length) >= 0 )
        return TRUE;
    return FALSE;
}


/**
 Escapes a string that may contain wildcard characters (%, _) and other
 problematic characters (", ', \n, etc). Like mysql_real_escape_string() but
 also including % and _.

 @param[in]   mysql         Pointer to MYSQL structure
 @param[out]  to            Buffer for escaped string
 @param[in]   to_length     Length of destination buffer, or 0 for "big enough"
 @param[in]   from          The string to escape
 @param[in]   length        The length of the string to escape

*/
ulong myodbc_escape_wildcard(MYSQL *mysql, char *to, ulong to_length,
                             const char *from, ulong length)
{
  CHARSET_INFO *charset_info= mysql->charset;
  const char *to_start= to;
  const char *end, *to_end=to_start + (to_length ? to_length-1 : 2*length);
  my_bool overflow= FALSE;
  my_bool use_mb_flag= use_mb(charset_info);
  for (end= from + length; from < end; from++)
  {
    char escape= 0;
    int tmp_length;
    if (use_mb_flag && (tmp_length= my_ismbchar(charset_info, from, end)))
    {
      if (to + tmp_length > to_end)
      {
        overflow= TRUE;
        break;
      }
      while (tmp_length--)
	*to++= *from++;
      from--;
      continue;
    }
    /*
     If the next character appears to begin a multi-byte character, we
     escape that first byte of that apparent multi-byte character. (The
     character just looks like a multi-byte character -- if it were actually
     a multi-byte character, it would have been passed through in the test
     above.)

     Without this check, we can create a problem by converting an invalid
     multi-byte character into a valid one. For example, 0xbf27 is not
     a valid GBK character, but 0xbf5c is. (0x27 = ', 0x5c = \)
    */
    if (use_mb_flag && (tmp_length= my_mbcharlen(charset_info, *from)) > 1)
      escape= *from;
    else
    switch (*from) {
    case 0:				/* Must be escaped for 'mysql' */
      escape= '0';
      break;
    case '\n':				/* Must be escaped for logs */
      escape= 'n';
      break;
    case '\r':
      escape= 'r';
      break;
    case '\\':
      escape= '\\';
      break;
    case '\'':
      escape= '\'';
      break;
    case '"':				/* Better safe than sorry */
      escape= '"';
      break;
    case '_':
      escape= '_';
      break;
    case '%':
      escape= '%';
      break;
    case '\032':			/* This gives problems on Win32 */
      escape= 'Z';
      break;
    }
    if (escape)
    {
      if (to + 2 > to_end)
      {
        overflow= TRUE;
        break;
      }
      *to++= '\\';
      *to++= escape;
    }
    else
    {
      if (to + 1 > to_end)
      {
        overflow= TRUE;
        break;
      }
      *to++= *from;
    }
  }
  *to= 0;
  return overflow ? (ulong)~0 : (ulong) (to - to_start);
}
