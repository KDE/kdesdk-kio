/* Copyright (C) 2003 Mickael Marchand <marchand@kde.org>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/

#ifndef _svn_H_
#define _svn_H_

#include <qstring.h>
#include <qcstring.h>

#include <kurl.h>
#include <kio/global.h>
#include <kio/slavebase.h>
#include <svn_pools.h>
#include <svn_auth.h>
#include <svn_client.h>
#include <svn_config.h>
#include <sys/stat.h>
#include <qvaluelist.h>

class QCString;

class kio_svnProtocol : public KIO::SlaveBase
{
	public:
		kio_svnProtocol(const QCString &pool_socket, const QCString &app_socket);
		virtual ~kio_svnProtocol();
		virtual void mimetype(const KURL& url);
		virtual void get(const KURL& url);
		virtual void listDir(const KURL& url);
		virtual void stat(const KURL& url);

	private:
		bool createUDSEntry( const QString& filename, const QString& user, long int size, bool isdir, time_t mtime, KIO::UDSEntry& entry);
		apr_pool_t *pool;
		svn_client_ctx_t **ctx;
		svn_auth_baton_t *auth_baton;
};

#endif
