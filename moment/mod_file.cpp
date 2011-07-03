/*  Moment Video Server - High performance media server
    Copyright (C) 2011 Dmitry Shatrov

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/


#include <libmary/types.h>
#include <cstring>

#include <libmary/module_init.h>

#include <moment/libmoment.h>


// TODO These header macros are the same as in rtmpt_server.cpp
#define MOMENT_FILE__HEADERS_DATE \
	Byte date_buf [timeToString_BufSize]; \
	Size const date_len = timeToString (Memory::forObject (date_buf), getUnixtime());

#define MOMENT_FILE__COMMON_HEADERS \
	"Server: Moment/1.0\r\n" \
	"Date: ", ConstMemory (date_buf, date_len), "\r\n" \
	"Connection: Keep-Alive\r\n" \
	"Cache-Control: no-cache\r\n"

#define MOMENT_FILE__OK_HEADERS(mime_type, content_length) \
	"HTTP/1.1 200 OK\r\n" \
	MOMENT_FILE__COMMON_HEADERS \
	"Content-Type: ", (mime_type), "\r\n" \
	"Content-Length: ", (content_length), "\r\n"

#define MOMENT_FILE__404_HEADERS(content_length) \
	"HTTP/1.1 404 Not found\r\n" \
	MOMENT_FILE__COMMON_HEADERS \
	"Content-Type: text/plain\r\n" \
	"Content-Length: ", (content_length), "\r\n"

#define MOMENT_FILE__400_HEADERS \
	"HTTP/1.1 400 Bad Request\r\n" \
	MOMENT_FILE__COMMON_HEADERS \
	"Content-Type: text/plain\r\n" \
	"Content-Length: 0\r\n"

#define MOMENT_FILE__500_HEADERS(content_length) \
	"HTTP/1.1 500 Internal Server Error\r\n" \
	MOMENT_FILE__COMMON_HEADERS \
	"Content-Type: text/plain\r\n" \
	"Content-Length: ", (content_length), "\r\n"


namespace Moment {

namespace {

PagePool *page_pool = NULL;
Ref<String> root_path;
Ref<String> path_prefix;

Result httpRequest (HttpRequest  * const mt_nonnull req,
		    Sender       * const mt_nonnull conn_sender,
		    Memory const &msg_body,
		    void        ** const mt_nonnull ret_msg_data,
		    void         * const cb_data)
{
    logD_ (_func, "HTTP request: ", req->getRequestLine());

  // TODO On Linux, we could do a better job with sendfile() or splice().

    ConstMemory file_path;
    {
	ConstMemory full_path = req->getFullPath();
	if (full_path.len() > 0
	    && full_path.mem() [0] == '/')
	{
	    full_path = full_path.region (1);
	}

	ConstMemory const prefix = path_prefix->mem();
	if (full_path.len() < prefix.len()
	    || memcmp (full_path.mem(), prefix.mem(), prefix.len()))
	{
	    logE_ (_func, "full_path \"", full_path, "\" does not match prefix \"", prefix, "\"");

	    MOMENT_FILE__HEADERS_DATE;
	    ConstMemory const reply_body = "500 Internal Server Error";
	    conn_sender->send (
		    page_pool,
		    MOMENT_FILE__500_HEADERS (reply_body.len()),
		    "\r\n",
		    reply_body);
	    conn_sender->flush ();

	    return Result::Success;
	}

	file_path = full_path.region (prefix.len());
    }

    Ref<String> const filename = makeString (root_path->mem(), "/", file_path);
    NativeFile native_file (filename->mem(),
			    0 /* open_flags */,
			    File::AccessMode::ReadOnly);
    if (exc) {
	logE_ (_func, "Could not open \"", filename, "\"");

	MOMENT_FILE__HEADERS_DATE;
	ConstMemory const reply_body = "404 Not Found";
	conn_sender->send (
		page_pool,
		MOMENT_FILE__404_HEADERS (reply_body.len()),
		"\r\n",
		reply_body);
	conn_sender->flush ();

	return Result::Success;
    }

    NativeFile::FileStat stat;
    if (!native_file.stat (&stat)) {
	logE_ (_func, "native_file.stat() failed: ", exc->toString());

	MOMENT_FILE__HEADERS_DATE;
	ConstMemory const reply_body = "500 Internal Server Error";
	conn_sender->send (
		page_pool,
		MOMENT_FILE__500_HEADERS (reply_body.len()),
		"\r\n",
		reply_body);
	conn_sender->flush ();

	return Result::Success;
    }

    MOMENT_FILE__HEADERS_DATE;
    conn_sender->send (
	    page_pool,
	    MOMENT_FILE__OK_HEADERS ("text/plain", stat.size),
	    "\r\n");

    PagePool::PageListHead page_list;

    Size total_sent = 0;
    Byte buf [65536];
    for (;;) {
	Size num_read;
	IoResult const res = native_file.read (Memory::forObject (buf), &num_read);
	if (res == IoResult::Error) {
	    logE_ (_func, "native_file.read() failed: ", exc->toString());
	    conn_sender->flush ();
	    conn_sender->closeAfterFlush ();
	    return Result::Success;
	}

	// TODO Double copy - not very smart.
	page_pool->getFillPages (&page_list, ConstMemory (buf, num_read));
	total_sent += num_read;

	if (res == IoResult::Eof)
	    break;
    }

    conn_sender->sendPages (page_pool, &page_list);
    conn_sender->flush ();

    assert (total_sent <= stat.size);
    if (total_sent != stat.size) {
	logE_ (_func, "File size mismatch: total_sent: ", total_sent, ", stat.size: ", stat.size);
	conn_sender->closeAfterFlush ();
	return Result::Success;
    }

    logD_ (_func, "done");
    return Result::Success;
}

HttpService::HttpHandler http_handler = {
    httpRequest,
    NULL /* httpMessageBody */
};

void momentFileInit ()
{
    MomentServer * const moment = MomentServer::getInstance();
    ServerApp * const server_app = moment->getServerApp();
    MConfig::Config * const config = moment->getConfig();
    HttpService * const http_service = moment->getHttpService ();

    page_pool = moment->getPagePool ();

    {
	ConstMemory const opt_name = "mod_file/enable";
	MConfig::Config::BooleanValue const enable = config->getBoolean (opt_name);
	if (enable == MConfig::Config::Boolean_Invalid) {
	    logE_ (_func, "Invalid value for ", opt_name, ": ", config->getString (opt_name));
	    return;
	}

	if (enable != MConfig::Config::Boolean_True) {
	    logI_ (_func, "Static HTTP content module is not enabled. "
		   "Set \"", opt_name, "\" option to \"y\" to enable.");
	    return;
	}
    }

    root_path = grab (new String (
	    config->getString_default ("mod_file/root_path", LIBMOMENT_PREFIX "/var/moment_www")));

    // Note: Don't forget to strip one leading slash, if any, when reading a config option.
    path_prefix = grab (new String ("file"));

    http_service->addHttpHandler (Cb<HttpService::HttpHandler> (&http_handler, NULL, NULL),
				  path_prefix->mem());
}

void momentFileUnload ()
{
}

}

}


namespace M {

void libMary_moduleInit ()
{
    Moment::momentFileInit ();
}

void libMary_moduleUnload()
{
    Moment::momentFileUnload ();
}

}

