/* This file is part of the KDE project
   Copyright (C) 2003 Mickael Marchand <marchand@kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/

#include <qcstring.h>
#include <qsocket.h>
#include <qdatetime.h>
#include <qbitarray.h>

#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <kapplication.h>
#include <kdebug.h>
#include <kmessagebox.h>
#include <kinstance.h>
#include <kglobal.h>
#include <kstandarddirs.h>
#include <klocale.h>
#include <kurl.h>
#include <ksock.h>
#include <dcopclient.h>
#include <qcstring.h>

#include <subversion-1/svn_sorts.h>
#include <subversion-1/svn_path.h>
#include <subversion-1/svn_utf.h>
#include <subversion-1/svn_ra.h>
#include <subversion-1/svn_time.h>

#include <kmimetype.h>
#include <qfile.h>

#include "svn.h"
#include <apr_portable.h>

using namespace KIO;

typedef struct
{
	/* Holds the directory that corresponds to the REPOS_URL at RA->open()
	 *      time. When callbacks specify a relative path, they are joined with
	 *           this base directory. */
	const char *base_dir;
	svn_wc_adm_access_t *base_access;

	/* An array of svn_client_commit_item_t * structures, present only
	 *      during working copy commits. */
	apr_array_header_t *commit_items;

	/* A hash of svn_config_t's, keyed off file name (i.e. the contents of
	 *      ~/.subversion/config end up keyed off of 'config'). */
	apr_hash_t *config;

	/* The pool to use for session-related items. */
	apr_pool_t *pool;

} svn_client__callback_baton_t;

static svn_error_t *
open_tmp_file (apr_file_t **fp,
               void *callback_baton,
               apr_pool_t *pool)
{
  svn_client__callback_baton_t *cb = (svn_client__callback_baton_t *) callback_baton;
  const char *truepath;
  const char *ignored_filename;

  if (cb->base_dir)
    truepath = apr_pstrdup (pool, cb->base_dir);
  else
    truepath = "";

  /* Tack on a made-up filename. */
  truepath = svn_path_join (truepath, "tempfile", pool);

  /* Open a unique file;  use APR_DELONCLOSE. */  
  SVN_ERR (svn_io_open_unique_file (fp, &ignored_filename,
                                    truepath, ".tmp", TRUE, pool));

  return SVN_NO_ERROR;
}

static svn_error_t *write_to_string(void *baton, const char *data, apr_size_t *len) {
	kbaton *tb = ( kbaton* )baton;
	svn_stringbuf_appendbytes(tb->target_string, data, *len);
	return SVN_NO_ERROR;
}

static int
compare_items_as_paths (const svn_sort__item_t*a, const svn_sort__item_t*b) {
  return svn_path_compare_paths ((const char *)a->key, (const char *)b->key);
}

kio_svnProtocol::kio_svnProtocol(const QCString &pool_socket, const QCString &app_socket)
	: SlaveBase("kio_svn", pool_socket, app_socket) {
		kdDebug(7128) << "kio_svnProtocol::kio_svnProtocol()" << endl;

		m_counter = 0;

		apr_initialize();
		// CleanUP ctx preventing crash in svn_client_update and other
		memset(&ctx, 0, sizeof(ctx));
		pool = svn_pool_create (NULL);

		svn_error_t *err = svn_client_create_context(&ctx, pool);
                if ( err ) {
                        kdDebug(7128) << "kio_svnProtocol::kio_svnProtocol() create_context ERROR" << endl;
                        error( KIO::ERR_SLAVE_DEFINED, err->message );
                        return;
                }

		err = svn_config_ensure (NULL,pool);
		if ( err ) {
			kdDebug(7128) << "kio_svnProtocol::kio_svnProtocol() configensure ERROR" << endl;
			error( KIO::ERR_SLAVE_DEFINED, err->message );
			return;
		}
		svn_config_get_config (&ctx->config, NULL, pool);

		ctx->log_msg_func = kio_svnProtocol::commitLogPrompt;
		ctx->log_msg_baton = this; //pass this so that we can get a dcopClient from it
		//TODO
		ctx->cancel_func = NULL;

		apr_array_header_t *providers = apr_array_make(pool, 9, sizeof(svn_auth_provider_object_t *));

		svn_auth_provider_object_t *provider;

		//disk cache
		svn_client_get_simple_provider(&provider,pool);
		APR_ARRAY_PUSH(providers, svn_auth_provider_object_t*) = provider;
		svn_client_get_username_provider(&provider,pool);
		APR_ARRAY_PUSH(providers, svn_auth_provider_object_t*) = provider;

		//interactive prompt
		svn_client_get_simple_prompt_provider (&provider,kio_svnProtocol::checkAuth,this,2,pool);
		APR_ARRAY_PUSH(providers, svn_auth_provider_object_t*) = provider;
		//we always ask user+pass, no need for a user only question
/*		svn_client_get_username_prompt_provider
 *		(&provider,kio_svnProtocol::checkAuth,this,2,pool);
		APR_ARRAY_PUSH(providers, svn_auth_provider_object_t*) = provider;*/
		
		//SSL disk cache, keep that one, because it does nothing bad :)
		svn_client_get_ssl_server_trust_file_provider (&provider, pool);
		APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
		svn_client_get_ssl_client_cert_file_provider (&provider, pool);
		APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
		svn_client_get_ssl_client_cert_pw_file_provider (&provider, pool);
		APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
		
		//SSL interactive prompt, where things get hard
		svn_client_get_ssl_server_trust_prompt_provider (&provider, kio_svnProtocol::trustSSLPrompt, NULL, pool);
		APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
		svn_client_get_ssl_client_cert_prompt_provider (&provider, kio_svnProtocol::clientCertSSLPrompt, NULL, 2, pool);
		APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;
		svn_client_get_ssl_client_cert_pw_prompt_provider (&provider, kio_svnProtocol::clientCertPasswdPrompt, NULL, 2, pool);
		APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

		svn_auth_open(&ctx->auth_baton, providers, pool);
}

kio_svnProtocol::~kio_svnProtocol(){
	kdDebug(7128) << "kio_svnProtocol::~kio_svnProtocol()" << endl;
	svn_pool_destroy(pool);
	apr_terminate();
}

void kio_svnProtocol::initNotifier(bool is_checkout, bool is_export, bool suppress_final_line, apr_pool_t *spool) {
	m_counter=0;//reset counter
	ctx->notify_func = kio_svnProtocol::notify;
	struct notify_baton *nb = ( struct notify_baton* )apr_palloc(spool, sizeof( *nb ) );
	nb->master = this;
	nb->received_some_change = FALSE;
	nb->sent_first_txdelta = FALSE;
	nb->is_checkout = is_checkout;
	nb->is_export = is_export;
	nb->suppress_final_line = suppress_final_line;
	nb->in_external = FALSE;
	nb->had_print_error = FALSE;
	nb->pool = svn_pool_create (spool);

	ctx->notify_baton = nb;
}

svn_error_t* kio_svnProtocol::checkAuth(svn_auth_cred_simple_t **cred, void *baton, const char *realm, const char *username, svn_boolean_t /*may_save*/, apr_pool_t *pool) {
	kdDebug(7128) << "kio_svnProtocol::checkAuth() for " << realm << endl;
	kio_svnProtocol *p = ( kio_svnProtocol* )baton;
	svn_auth_cred_simple_t *ret = (svn_auth_cred_simple_t*)apr_pcalloc (pool, sizeof (*ret));
	
//	p->info.keepPassword = true;
	p->info.verifyPath=true;
	kdDebug(7128 ) << "auth current URL : " << p->myURL.url() << endl;
	p->info.url = p->myURL;
	p->info.username = username; //( const char* )svn_auth_get_parameter( p->ctx->auth_baton, SVN_AUTH_PARAM_DEFAULT_USERNAME );
//	if ( !p->checkCachedAuthentication( p->info ) ){
		p->openPassDlg( p->info );
//	}
	ret->username = apr_pstrdup(pool, p->info.username.utf8());
	ret->password = apr_pstrdup(pool, p->info.password.utf8());
	ret->may_save = true;
	*cred = ret;
	return SVN_NO_ERROR;
}

void kio_svnProtocol::recordCurrentURL(const KURL& url) {
	myURL = url;
}

//don't implement mimeType() until we don't need to download the whole file

void kio_svnProtocol::get(const KURL& url ){
	kdDebug(7128) << "kio_svn::get(const KURL& url)" << endl ;

	QString remoteServer = url.host();
	infoMessage(i18n("Looking for %1...").arg( remoteServer ) );

	apr_pool_t *subpool = svn_pool_create (pool);
	kbaton *bt = (kbaton*)apr_pcalloc(subpool, sizeof(*bt));
	bt->target_string = svn_stringbuf_create("", subpool);
	bt->string_stream = svn_stream_create(bt,subpool);
	svn_stream_set_write(bt->string_stream,write_to_string);

	QString target = makeSvnURL( url );
	kdDebug(7128) << "SvnURL: " << target << endl;
	recordCurrentURL( KURL( target ) );
	
	//find the requested revision
	svn_opt_revision_t rev;
	svn_opt_revision_t endrev;
	int idx = target.findRev( "?rev=" );
	if ( idx != -1 ) {
		QString revstr = target.mid( idx+5 );
#if 0
		kdDebug(7128) << "revision string found " << revstr  << endl;
		if ( revstr == "HEAD" ) {
			rev.kind = svn_opt_revision_head;
			kdDebug(7128) << "revision searched : HEAD" << endl;
		} else {
			rev.kind = svn_opt_revision_number;
			rev.value.number = revstr.toLong();
			kdDebug(7128) << "revision searched : " << rev.value.number << endl;
		}
#endif
		svn_opt_parse_revision( &rev, &endrev, revstr.utf8(), subpool );
		target = target.left( idx );
		kdDebug(7128) << "new target : " << target << endl;
	} else {
		kdDebug(7128) << "no revision given. searching HEAD " << endl;
		rev.kind = svn_opt_revision_head;
	}
	initNotifier(false, false, false, subpool);

	svn_error_t *err = svn_client_cat (bt->string_stream, svn_path_canonicalize( target.utf8(),subpool ),&rev,ctx, subpool);
	if ( err ) {
		error( KIO::ERR_SLAVE_DEFINED, err->message );
		svn_pool_destroy( subpool );
		return;
	}

	// Send the mimeType as soon as it is known
	QByteArray *cp = new QByteArray();
	cp->setRawData( bt->target_string->data, bt->target_string->len );
	KMimeType::Ptr mt = KMimeType::findByContent(*cp);
	kdDebug(7128) << "KMimeType returned : " << mt->name() << endl;
	mimeType( mt->name() );

	totalSize(bt->target_string->len);

	//send data
	data(*cp);

	data(QByteArray()); // empty array means we're done sending the data
	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::stat(const KURL & url){
	kdDebug(7128) << "kio_svn::stat(const KURL& url) : " << url.url() << endl ;

	void *ra_baton, *session;
	svn_ra_plugin_t *ra_lib;
	svn_node_kind_t kind;
	apr_pool_t *subpool = svn_pool_create (pool);

	QString target = makeSvnURL( url);
	kdDebug(7128) << "SvnURL: " << target << endl;
	recordCurrentURL( KURL( target ) );
	
	//find the requested revision
	svn_opt_revision_t rev;
	svn_opt_revision_t endrev;
	int idx = target.findRev( "?rev=" );
	if ( idx != -1 ) {
		QString revstr = target.mid( idx+5 );
#if 0
		kdDebug(7128) << "revision string found " << revstr  << endl;
		if ( revstr == "HEAD" ) {
			rev.kind = svn_opt_revision_head;
			kdDebug(7128) << "revision searched : HEAD" << endl;
		} else {
			rev.kind = svn_opt_revision_number;
			rev.value.number = revstr.toLong();
			kdDebug(7128) << "revision searched : " << rev.value.number << endl;
		}
#endif
		svn_opt_parse_revision( &rev, &endrev, revstr.utf8( ), subpool );
		target = target.left( idx );
		kdDebug(7128) << "new target : " << target << endl;
	} else {
		kdDebug(7128) << "no revision given. searching HEAD " << endl;
		rev.kind = svn_opt_revision_head;
	}

	//init
	svn_error_t *err = svn_ra_init_ra_libs(&ra_baton,subpool);
	if ( err ) {
		kdDebug(7128) << "init RA libs failed : " << err->message << endl;
		return;
	}
	//find RA libs
	err = svn_ra_get_ra_library(&ra_lib,ra_baton,svn_path_canonicalize( target.utf8(), subpool ),subpool);
	if ( err ) {
		kdDebug(7128) << "RA get libs failed : " << err->message << endl;
		return;
	}
	kdDebug(7128) << "RA init completed" << endl;
	
	//start session
	svn_ra_callbacks_t *cbtable = (svn_ra_callbacks_t*)apr_pcalloc(subpool, sizeof(*cbtable));	
	kio_svn_callback_baton_t *callbackbt = (kio_svn_callback_baton_t*)apr_pcalloc(subpool, sizeof( *callbackbt ));

	cbtable->open_tmp_file = open_tmp_file;
	cbtable->get_wc_prop = NULL;
	cbtable->set_wc_prop = NULL;
	cbtable->push_wc_prop = NULL;
	cbtable->auth_baton = ctx->auth_baton;

	callbackbt->base_dir = target.utf8();
	callbackbt->pool = subpool;
	callbackbt->config = ctx->config;
	
	err = ra_lib->open(&session,svn_path_canonicalize( target.utf8(), subpool ),cbtable,callbackbt,ctx->config,subpool);
	if ( err ) {
		kdDebug(7128)<< "Open session " << err->message << endl;
		return;
	}
	kdDebug(7128) << "Session opened to " << target << endl;
	//find number for HEAD
	if (rev.kind == svn_opt_revision_head) {
		err = ra_lib->get_latest_revnum(session,&rev.value.number,subpool);
		if ( err ) {
			kdDebug(7128)<< "Latest RevNum " << err->message << endl;
			return;
		}
		kdDebug(7128) << "Got rev " << rev.value.number << endl;
	}
	
	//get it
	ra_lib->check_path(session,"",rev.value.number,&kind,subpool);
	kdDebug(7128) << "Checked Path" << endl;
	UDSEntry entry;
	switch ( kind ) {
		case svn_node_file:
			kdDebug(7128) << "::stat result : file" << endl;
			createUDSEntry(url.filename(),"",0,false,0,entry);
			statEntry( entry );
			break;
		case svn_node_dir:
			kdDebug(7128) << "::stat result : directory" << endl;
			createUDSEntry(url.filename(),"",0,true,0,entry);
			statEntry( entry );
			break;
		case svn_node_unknown:
		case svn_node_none:
			//error XXX
		default:
			kdDebug(7128) << "::stat result : UNKNOWN ==> WOW :)" << endl;
			;
	}
	finished();
	svn_pool_destroy( subpool );
}

void kio_svnProtocol::listDir(const KURL& url){
	kdDebug(7128) << "kio_svn::listDir(const KURL& url) : " << url.url() << endl ;

	apr_pool_t *subpool = svn_pool_create (pool);
	apr_hash_t *dirents;

	QString target = makeSvnURL( url);
	kdDebug(7128) << "SvnURL: " << target << endl;
	recordCurrentURL( KURL( target ) );
	
	//find the requested revision
	svn_opt_revision_t rev;
	svn_opt_revision_t endrev;
	int idx = target.findRev( "?rev=" );
	if ( idx != -1 ) {
		QString revstr = target.mid( idx+5 );
		svn_opt_parse_revision( &rev, &endrev, revstr.utf8(), subpool );
#if 0
		kdDebug(7128) << "revision string found " << revstr  << endl;
		if ( revstr == "HEAD" ) {
			rev.kind = svn_opt_revision_head;
			kdDebug(7128) << "revision searched : HEAD" << endl;
		} else {
			rev.kind = svn_opt_revision_number;
			rev.value.number = revstr.toLong();
			kdDebug(7128) << "revision searched : " << rev.value.number << endl;
		}
#endif
		target = target.left( idx );
		kdDebug(7128) << "new target : " << target << endl;
	} else {
		kdDebug(7128) << "no revision given. searching HEAD " << endl;
		rev.kind = svn_opt_revision_head;
	}

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_ls (&dirents, svn_path_canonicalize( target.utf8(), subpool ), &rev, false, ctx, subpool);
	if ( err ) {
		error( KIO::ERR_SLAVE_DEFINED, err->message );
		svn_pool_destroy( subpool );
		return;
	}

  apr_array_header_t *array;
  int i;

  array = svn_sort__hash (dirents, compare_items_as_paths, subpool);
  
  UDSEntry entry;
  for (i = 0; i < array->nelts; ++i) {
	  entry.clear();
      const char *utf8_entryname, *native_entryname;
      svn_dirent_t *dirent;
      svn_sort__item_t *item;
     
      item = &APR_ARRAY_IDX (array, i, svn_sort__item_t);

      utf8_entryname = (const char*)item->key;

      dirent = (svn_dirent_t*)apr_hash_get (dirents, utf8_entryname, item->klen);

      svn_utf_cstring_from_utf8 (&native_entryname, utf8_entryname, subpool);
			const char *native_author = NULL;

			//XXX BUGGY
/*			apr_time_exp_t timexp;
			apr_time_exp_lt(&timexp, dirent->time);
			apr_os_exp_time_t *ostime;
			apr_os_exp_time_get( &ostime, &timexp);

			time_t mtime = mktime( ostime );*/

			if (dirent->last_author)
				svn_utf_cstring_from_utf8 (&native_author, dirent->last_author, subpool);

			if ( createUDSEntry(QString( native_entryname ), QString( native_author ), dirent->size,
						dirent->kind==svn_node_dir ? true : false, 0, entry) )
				listEntry( entry, false );
	}
	listEntry( entry, true );

	finished();
	svn_pool_destroy (subpool);
}

bool kio_svnProtocol::createUDSEntry( const QString& filename, const QString& user, long long int size, bool isdir, time_t mtime, UDSEntry& entry) {
	kdDebug(7128) << "MTime : " << ( long )mtime << endl;
	kdDebug(7128) << "UDS filename : " << filename << endl;
	UDSAtom atom;
	atom.m_uds = KIO::UDS_NAME;
	atom.m_str = filename;
	entry.append( atom );

	atom.m_uds = KIO::UDS_FILE_TYPE;
	atom.m_long = isdir ? S_IFDIR : S_IFREG;
	entry.append( atom );

	atom.m_uds = KIO::UDS_SIZE;
	atom.m_long = size;
	entry.append( atom );
	
	atom.m_uds = KIO::UDS_MODIFICATION_TIME;
	atom.m_long = mtime;
	entry.append( atom );
	
	atom.m_uds = KIO::UDS_USER;
	atom.m_str = user;
	entry.append( atom );

	return true;
}

void kio_svnProtocol::copy(const KURL & src, const KURL& dest, int /*permissions*/, bool /*overwrite*/) {
	kdDebug(7128) << "kio_svnProtocol::copy() Source : " << src.url() << " Dest : " << dest.url() << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	svn_client_commit_info_t *commit_info = NULL;

	KURL nsrc = src;
	KURL ndest = dest;
	nsrc.setProtocol( chooseProtocol( src.protocol() ) );
	ndest.setProtocol( chooseProtocol( dest.protocol() ) );
	QString srcsvn = nsrc.url();
	QString destsvn = ndest.url();
	
	recordCurrentURL( nsrc );

	//find the requested revision
	svn_opt_revision_t rev;
	int idx = srcsvn.findRev( "?rev=" );
	if ( idx != -1 ) {
		QString revstr = srcsvn.mid( idx+5 );
		kdDebug(7128) << "revision string found " << revstr  << endl;
		if ( revstr == "HEAD" ) {
			rev.kind = svn_opt_revision_head;
			kdDebug(7128) << "revision searched : HEAD" << endl;
		} else {
			rev.kind = svn_opt_revision_number;
			rev.value.number = revstr.toLong();
			kdDebug(7128) << "revision searched : " << rev.value.number << endl;
		}
		srcsvn = srcsvn.left( idx );
		kdDebug(7128) << "new src : " << srcsvn << endl;
	} else {
		kdDebug(7128) << "no revision given. searching HEAD " << endl;
		rev.kind = svn_opt_revision_head;
	}

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_copy(&commit_info, srcsvn.utf8(), &rev, destsvn.utf8(), ctx, subpool);
	if ( err ) {
		error( KIO::ERR_SLAVE_DEFINED, err->message );
	}
	
	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::mkdir( const KURL::List& list, int /*permissions*/ ) {
	kdDebug(7128) << "kio_svnProtocol::mkdir(LIST) : " << list << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	svn_client_commit_info_t *commit_info = NULL;

	recordCurrentURL( list[ 0 ] );
	
	apr_array_header_t *targets = apr_array_make(subpool, list.count()+1, sizeof(const char *));

	KURL::List::const_iterator it = list.begin(), end = list.end();
	for ( ; it != end; ++it ) {
		QString cur = makeSvnURL( *it );
		kdDebug( 7128 ) << "kio_svnProtocol::mkdir raw url for subversion : " << cur << endl;
		const char *_target = apr_pstrdup( subpool, svn_path_canonicalize( apr_pstrdup( subpool, cur.utf8() ), subpool ) );
		(*(( const char ** )apr_array_push(( apr_array_header_t* )targets)) ) = _target;
	}

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_mkdir(&commit_info,targets,ctx,subpool);
	if ( err ) {
		error( KIO::ERR_COULD_NOT_MKDIR, err->message );
	}
	
	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::mkdir( const KURL& url, int /*permissions*/ ) {
	kdDebug(7128) << "kio_svnProtocol::mkdir() : " << url.url() << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	svn_client_commit_info_t *commit_info = NULL;

	QString target = makeSvnURL( url);
	kdDebug(7128) << "SvnURL: " << target << endl;
	recordCurrentURL( KURL( target ) );
	
	apr_array_header_t *targets = apr_array_make(subpool, 2, sizeof(const char *));
	(*(( const char ** )apr_array_push(( apr_array_header_t* )targets)) ) = apr_pstrdup( subpool, target.utf8() );

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_mkdir(&commit_info,targets,ctx,subpool);
	if ( err ) {
		error( KIO::ERR_COULD_NOT_MKDIR, err->message );
	}
	
	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::del( const KURL& url, bool /*isfile*/ ) {
	kdDebug(7128) << "kio_svnProtocol::del() : " << url.url() << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	svn_client_commit_info_t *commit_info = NULL;

	QString target = makeSvnURL(url);
	kdDebug(7128) << "SvnURL: " << target << endl;
	recordCurrentURL( KURL( target ) );
	
	apr_array_header_t *targets = apr_array_make(subpool, 2, sizeof(const char *));
	(*(( const char ** )apr_array_push(( apr_array_header_t* )targets)) ) = apr_pstrdup( subpool, target.utf8() );

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_delete(&commit_info,targets,false/*force remove locally modified files in wc*/,ctx,subpool);
	if ( err ) {
		error( KIO::ERR_CANNOT_DELETE, err->message );
	}
	
	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::rename(const KURL& src, const KURL& dest, bool /*overwrite*/) {
	kdDebug(7128) << "kio_svnProtocol::rename() Source : " << src.url() << " Dest : " << dest.url() << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	svn_client_commit_info_t *commit_info = NULL;

	KURL nsrc = src;
	KURL ndest = dest;
	nsrc.setProtocol( chooseProtocol( src.protocol() ) );
	ndest.setProtocol( chooseProtocol( dest.protocol() ) );
	QString srcsvn = nsrc.url();
	QString destsvn = ndest.url();
	
	recordCurrentURL( nsrc );

	//find the requested revision
	svn_opt_revision_t rev;
	int idx = srcsvn.findRev( "?rev=" );
	if ( idx != -1 ) {
		QString revstr = srcsvn.mid( idx+5 );
		kdDebug(7128) << "revision string found " << revstr  << endl;
		if ( revstr == "HEAD" ) {
			rev.kind = svn_opt_revision_head;
			kdDebug(7128) << "revision searched : HEAD" << endl;
		} else {
			rev.kind = svn_opt_revision_number;
			rev.value.number = revstr.toLong();
			kdDebug(7128) << "revision searched : " << rev.value.number << endl;
		}
		srcsvn = srcsvn.left( idx );
		kdDebug(7128) << "new src : " << srcsvn << endl;
	} else {
		kdDebug(7128) << "no revision given. searching HEAD " << endl;
		rev.kind = svn_opt_revision_head;
	}

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_move(&commit_info, srcsvn.utf8(), &rev, destsvn.utf8(), false/*force remove locally modified files in wc*/, ctx, subpool);
	if ( err ) {
		error( KIO::ERR_CANNOT_RENAME, err->message );
	}
	
	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::special( const QByteArray& data ) {
	kdDebug(7128) << "kio_svnProtocol::special" << endl;

	QDataStream stream(data, IO_ReadOnly);
	int tmp;

	stream >> tmp;
	kdDebug(7128) << "kio_svnProtocol::special " << tmp << endl;

	switch ( tmp ) {
		case SVN_CHECKOUT: 
			{
				KURL repository, wc;
				int revnumber;
				QString revkind;
				stream >> repository;
				stream >> wc;
				stream >> revnumber;
				stream >> revkind;
				kdDebug(7128) << "kio_svnProtocol CHECKOUT from " << repository.url() << " to " << wc.url() << " at " << revnumber << " or " << revkind << endl;
				checkout( repository, wc, revnumber, revkind );
				break;
			}
		case SVN_UPDATE: 
			{
				KURL wc;
				int revnumber;
				QString revkind;
				stream >> wc;
				stream >> revnumber;
				stream >> revkind;
				kdDebug(7128) << "kio_svnProtocol UPDATE " << wc.url() << " at " << revnumber << " or " << revkind << endl;
				update(wc, revnumber, revkind );
				break;
			}
		case SVN_COMMIT: 
			{
				KURL::List wclist;
				while ( !stream.atEnd() ) {
					KURL tmp;
					stream >> tmp;
					wclist << tmp;
				}
				kdDebug(7128) << "kio_svnProtocol COMMIT" << endl;
				commit( wclist );
				break;
			}
		case SVN_LOG: 
			{
				kdDebug(7128) << "kio_svnProtocol LOG" << endl;
				int revstart, revend;
				QString revkindstart, revkindend;
				KURL::List targets;
				stream >> revstart;
				stream >> revkindstart;
				stream >> revend;
				stream >> revkindend;
				while ( !stream.atEnd() ) {
					KURL tmp;
					stream >> tmp;
					targets << tmp;
				}
				svn_log( revstart, revkindstart, revend, revkindend, targets );
				break;
			}
		case SVN_IMPORT: 
			{
				KURL wc,repos;
				stream >> repos;
				stream >> wc;
				kdDebug(7128) << "kio_svnProtocol IMPORT" << endl;
				import(repos,wc);
				break;
			}
		case SVN_ADD: 
			{
				KURL wc;
				stream >> wc;
				kdDebug(7128) << "kio_svnProtocol ADD" << endl;
				add(wc);
				break;
			}
		case SVN_DEL: 
			{
				KURL::List wclist;
				while ( !stream.atEnd() ) {
					KURL tmp;
					stream >> tmp;
					wclist << tmp;
				}
				kdDebug(7128) << "kio_svnProtocol DEL" << endl;
				wc_delete(wclist);
				break;
			}
		case SVN_REVERT: 
			{
				KURL::List wclist;
				while ( !stream.atEnd() ) {
					KURL tmp;
					stream >> tmp;
					wclist << tmp;
				}
				kdDebug(7128) << "kio_svnProtocol REVERT" << endl;
				wc_revert(wclist);
				break;
			}
		case SVN_STATUS: 
			{
				KURL wc;
				int checkRepos=false;
				int fullRecurse=false;
				stream >> wc;
				stream >> checkRepos;
				stream >> fullRecurse;
				kdDebug(7128) << "kio_svnProtocol STATUS" << endl;
				wc_status(wc,checkRepos,fullRecurse);
				break;
			}
		case SVN_MKDIR:
			{
				KURL::List list;
				stream >> list;
				kdDebug(7128) << "kio_svnProtocol MKDIR" << endl;
				mkdir(list,0);
				break;
			}
		case SVN_RESOLVE:
			{
				KURL url;
				bool recurse;
				stream >> url;
				stream >> recurse;
				kdDebug(7128) << "kio_svnProtocol RESOLVE" << endl;
				wc_resolve(url,recurse);
				break;
			}
		case SVN_SWITCH:
			{
				KURL wc,url;
				bool recurse;
				int revnumber;
				QString revkind;
				stream >> wc;
				stream >> url;
				stream >> recurse;
				stream >> revnumber;
				stream >> revkind;
				kdDebug(7128) << "kio_svnProtocol SWITCH" << endl;
				svn_switch(wc,url,revnumber,revkind,recurse);
				break;
			}
		case SVN_DIFF:
			{
				KURL url1,url2;
				int rev1, rev2;
				bool recurse;
				QString revkind1, revkind2;
				stream >> url1;
				stream >> url2;
				stream >> rev1;
				stream >> revkind1;
				stream >> rev2;
				stream >> revkind2;
				stream >> recurse;
				kdDebug(7128) << "kio_svnProtocol DIFF" << endl;
				svn_diff(url1,url2,rev1,rev2,revkind1,revkind2,recurse);
				break;
			}
		default:
			{
				kdDebug(7128) << "kio_svnProtocol DEFAULT" << endl;
				break;
			}
	}
}

void kio_svnProtocol::popupMessage( const QString& message ) {
	QByteArray params;
	QDataStream stream(params, IO_WriteOnly);
	stream << message;	

	if ( !dcopClient()->send( "kded","ksvnd","popupMessage(QString)", params ) )
		kdWarning() << "Communication with KDED:KSvnd failed" << endl;
}

void kio_svnProtocol::svn_log( int revstart, const QString& revkindstart, int revend, const QString& revkindend, const KURL::List& targets ) {
	kdDebug(7128) << "kio_svn::log : " << targets << " from revision " << revstart << " or " << revkindstart << " to "
		" revision " << revend << " or " << revkindend
		<< endl;

	apr_pool_t *subpool = svn_pool_create (pool);

	svn_opt_revision_t rev1 = createRevision( revstart, revkindstart, subpool );
	svn_opt_revision_t rev2 = createRevision( revend, revkindend, subpool );

	//TODO

	finished();
	svn_pool_destroy (subpool);
}

svn_opt_revision_t kio_svnProtocol::createRevision( int revision, const QString& revkind, apr_pool_t *pool ) {
	svn_opt_revision_t result,endrev;

	if ( revision != -1 ) {
		result.value.number = revision;
		result.kind = svn_opt_revision_number;
	} else if ( revkind == "WORKING" ) {
		result.kind = svn_opt_revision_working;
	} else if ( revkind == "BASE" ) {
		result.kind = svn_opt_revision_base;
	} else if ( !revkind.isNull() ) {
		svn_opt_parse_revision(&result,&endrev,revkind.utf8(),pool);
	}
	return result;
}

void kio_svnProtocol::svn_diff(const KURL & url1, const KURL& url2,int rev1, int rev2,const QString& revkind1,const QString& revkind2,bool recurse) {
	kdDebug(7128) << "kio_svn::diff : " << url1.path() << " at revision " << rev1 << " or " << revkind1 << " with "
		<< url2.path() << " at revision " << rev2 << " or " << revkind2
		<< endl ;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	apr_array_header_t *options = svn_cstring_split( "", "\t\r\n", TRUE, subpool );

	KURL nurl1 = url1;
	KURL nurl2 = url2;
	nurl1.setProtocol( chooseProtocol( url1.protocol() ) ); //svn+https -> https for eg
	nurl2.setProtocol( chooseProtocol( url2.protocol() ) );
	recordCurrentURL( nurl1 );
	QString source = makeSvnURL( nurl1 );
	QString target = makeSvnURL( nurl2 );

	const char *path1 = svn_path_canonicalize( apr_pstrdup( subpool, source.utf8() ), subpool );
	const char *path2 = svn_path_canonicalize( apr_pstrdup( subpool, target.utf8() ), subpool );
	//remove file:/// so we can diff for working copies, needs a better check (so we support URL for file:/// _repositories_ )
	if ( nurl1.protocol() == "file" ) {
		path1 = svn_path_canonicalize( apr_pstrdup( subpool, nurl1.path().utf8() ), subpool );
	}
	if ( nurl2.protocol() == "file" ) {
		path2 = svn_path_canonicalize( apr_pstrdup( subpool, nurl2.path().utf8() ), subpool );
	}
	kdDebug( 7128 ) << "1 : " << path1 << " 2: " << path2 << endl;

	svn_opt_revision_t revision1,revision2;
	revision1 = createRevision(rev1, revkind1, subpool);
	revision2 = createRevision(rev2, revkind2, subpool);

	char *templ;
    templ = apr_pstrdup ( subpool, "/tmp/tmpfile_XXXXXX" );
	apr_file_t *outfile = NULL;
	apr_file_mktemp( &outfile, templ , APR_READ|APR_WRITE|APR_CREATE|APR_TRUNCATE, subpool );

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_diff (options, path1, &revision1, path2, &revision2, recurse, false, true, outfile, NULL, ctx, subpool);
	if ( err )
		error( KIO::ERR_SLAVE_DEFINED, err->message );
	//read the content of the outfile now
	QStringList tmp;
	apr_file_close(outfile);
	QFile file(templ);
	if ( file.open(  IO_ReadOnly ) ) {
		QTextStream stream(  &file );
		QString line;
		while ( !stream.atEnd() ) {
			line = stream.readLine();
			tmp << line;
		}
		file.close();
	}
	for ( QStringList::Iterator itt = tmp.begin(); itt != tmp.end(); itt++ ) {
		setMetaData(QString::number( m_counter ).rightJustify( 10,'0' )+ "diffresult", ( *itt ) );
		m_counter++;
	}
	//delete temp file
	file.remove();

	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::svn_switch( const KURL& wc, const KURL& repos, int revnumber, const QString& revkind, bool recurse) {
	kdDebug(7128) << "kio_svn::switch : " << wc.path() << " at revision " << revnumber << " or " << revkind << endl ;

	apr_pool_t *subpool = svn_pool_create (pool);

	KURL nurl = repos;
	KURL dest = wc;
	nurl.setProtocol( chooseProtocol( repos.protocol() ) );
	dest.setProtocol( "file" );
	recordCurrentURL( nurl );
	QString source = dest.path();
	QString target = makeSvnURL( repos );

	const char *path = svn_path_canonicalize( apr_pstrdup( subpool, source.utf8() ), subpool );
	const char *url = svn_path_canonicalize( apr_pstrdup( subpool, target.utf8() ), subpool );

	svn_opt_revision_t rev = createRevision( revnumber, revkind, subpool );
	
	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_switch (NULL/*result revision*/, path, url, &rev, recurse, ctx, subpool);
	if ( err )
		error( KIO::ERR_SLAVE_DEFINED, err->message );

	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::update( const KURL& wc, int revnumber, const QString& revkind ) {
	kdDebug(7128) << "kio_svn::update : " << wc.path() << " at revision " << revnumber << " or " << revkind << endl ;

	apr_pool_t *subpool = svn_pool_create (pool);
	KURL dest = wc;
	dest.setProtocol( "file" );
	QString target = dest.path();
	recordCurrentURL( dest );
	
	svn_opt_revision_t rev = createRevision( revnumber, revkind, subpool );

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_update (NULL, svn_path_canonicalize( target.utf8(), subpool ), &rev, true, ctx, subpool);
	if ( err ) 
		error( KIO::ERR_SLAVE_DEFINED, err->message );

	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::import( const KURL& repos, const KURL& wc ) {
	kdDebug(7128) << "kio_svnProtocol::import() : " << wc.url() << " into " << repos.url() << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	svn_client_commit_info_t *commit_info = NULL;
	bool nonrecursive = false;

	KURL nurl = repos;
	KURL dest = wc;
	nurl.setProtocol( chooseProtocol( repos.protocol() ) );
	dest.setProtocol( "file" );
	recordCurrentURL( nurl );
	dest.cleanPath( true ); // remove doubled '/'
	QString source = dest.path(-1);
	QString target = makeSvnURL( repos );

	const char *path = svn_path_canonicalize( apr_pstrdup( subpool, source.utf8() ), subpool );
	const char *url = svn_path_canonicalize( apr_pstrdup( subpool, target.utf8() ), subpool );

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_import(&commit_info,path,url,nonrecursive,ctx,subpool);
	if ( err )
		error( KIO::ERR_SLAVE_DEFINED, err->message );
	
	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::checkout( const KURL& repos, const KURL& wc, int revnumber, const QString& revkind ) {
	kdDebug(7128) << "kio_svn::checkout : " << repos.url() << " into " << wc.path() << " at revision " << revnumber << " or " << revkind << endl ;

	apr_pool_t *subpool = svn_pool_create (pool);
	KURL nurl = repos;
	KURL dest = wc;
	nurl.setProtocol( chooseProtocol( repos.protocol() ) );
	dest.setProtocol( "file" );
	QString target = makeSvnURL( repos );
	recordCurrentURL( nurl );
	QString dpath = dest.path();
	
	//find the requested revision
	svn_opt_revision_t rev = createRevision( revnumber, revkind, subpool );

	initNotifier(true, false, false, subpool);
	svn_error_t *err = svn_client_checkout (NULL/* rev actually checkedout */, svn_path_canonicalize( target.utf8(), subpool ), svn_path_canonicalize ( dpath.utf8(), subpool ), &rev, true, ctx, subpool);
	if ( err )
		error( KIO::ERR_SLAVE_DEFINED, err->message );

	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::commit(const KURL::List& wc) {
	kdDebug(7128) << "kio_svnProtocol::commit() : " << wc << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	svn_client_commit_info_t *commit_info = NULL;
	bool nonrecursive = false;

	apr_array_header_t *targets = apr_array_make(subpool, 1+wc.count(), sizeof(const char *));

	for ( QValueListConstIterator<KURL> it = wc.begin(); it != wc.end() ; ++it ) {
		KURL nurl = *it;
		nurl.setProtocol( "file" );
		recordCurrentURL( nurl );
		(*(( const char ** )apr_array_push(( apr_array_header_t* )targets)) ) = svn_path_canonicalize( nurl.path().utf8(), subpool );
	}

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_commit(&commit_info,targets,nonrecursive,ctx,subpool);
	if ( err )
		error( KIO::ERR_SLAVE_DEFINED, err->message );

	if ( commit_info ) {
		for ( QValueListConstIterator<KURL> it = wc.begin(); it != wc.end() ; ++it ) {
			KURL nurl = *it;
			nurl.setProtocol( "file" );

			QString userstring = i18n ( "Nothing to commit." );
			if ( SVN_IS_VALID_REVNUM( commit_info->revision ) )
				userstring = i18n( "Committed revision %1." ).arg(commit_info->revision);
			setMetaData(QString::number( m_counter ).rightJustify( 10,'0' )+ "path", nurl.path() );
			setMetaData(QString::number( m_counter ).rightJustify( 10,'0' )+ "action", "0" ); 
			setMetaData(QString::number( m_counter ).rightJustify( 10,'0' )+ "kind", "0" );
			setMetaData(QString::number( m_counter ).rightJustify( 10,'0' )+ "mime_t", "" );
			setMetaData(QString::number( m_counter ).rightJustify( 10,'0' )+ "content", "0" );
			setMetaData(QString::number( m_counter ).rightJustify( 10,'0' )+ "prop", "0" );
			setMetaData(QString::number( m_counter ).rightJustify( 10,'0' )+ "rev" , QString::number( commit_info->revision ) );
			setMetaData(QString::number( m_counter ).rightJustify( 10,'0' )+ "string", userstring );
			m_counter++;
		}
	}

	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::add(const KURL& wc) {
	kdDebug(7128) << "kio_svnProtocol::add() : " << wc.url() << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	bool nonrecursive = false;

	KURL nurl = wc;
	nurl.setProtocol( "file" );
	QString target = nurl.url();
	recordCurrentURL( nurl );

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_add(svn_path_canonicalize( nurl.path().utf8(), subpool ),nonrecursive,ctx,subpool);
	if ( err )
		error( KIO::ERR_SLAVE_DEFINED, err->message );
	
	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::wc_delete(const KURL::List& wc) {
	kdDebug(7128) << "kio_svnProtocol::wc_delete() : " << wc << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	svn_client_commit_info_t *commit_info = NULL;
	bool nonrecursive = false;

	apr_array_header_t *targets = apr_array_make(subpool, 1+wc.count(), sizeof(const char *));

	for ( QValueListConstIterator<KURL> it = wc.begin(); it != wc.end() ; ++it ) {
		KURL nurl = *it;
		nurl.setProtocol( "file" );
		recordCurrentURL( nurl );
		(*(( const char ** )apr_array_push(( apr_array_header_t* )targets)) ) = svn_path_canonicalize( nurl.path().utf8(), subpool );
	}

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_delete(&commit_info,targets,nonrecursive,ctx,subpool);

	if ( err )
		error( KIO::ERR_SLAVE_DEFINED, err->message );
	
	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::wc_revert(const KURL::List& wc) {
	kdDebug(7128) << "kio_svnProtocol::revert() : " << wc << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	bool nonrecursive = false;

	apr_array_header_t *targets = apr_array_make(subpool, 1 + wc.count(), sizeof(const char *));

	for ( QValueListConstIterator<KURL> it = wc.begin(); it != wc.end() ; ++it ) {
		KURL nurl = *it;
		nurl.setProtocol( "file" );
		recordCurrentURL( nurl );
		(*(( const char ** )apr_array_push(( apr_array_header_t* )targets)) ) = svn_path_canonicalize( nurl.path().utf8(), subpool );
	}

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_revert(targets,nonrecursive,ctx,subpool);
	if ( err )
		error( KIO::ERR_SLAVE_DEFINED, err->message );
	
	finished();
	svn_pool_destroy (subpool);
}

void kio_svnProtocol::wc_status(const KURL& wc, bool checkRepos, bool fullRecurse, bool getAll, int revnumber, const QString& revkind) {
	kdDebug(7128) << "kio_svnProtocol::status() : " << wc.url() << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);
	svn_revnum_t result_rev;
	bool no_ignore = FALSE;

	KURL nurl = wc;
	nurl.setProtocol( "file" );
	recordCurrentURL( nurl );

	svn_opt_revision_t rev = createRevision( revnumber, revkind, subpool );

	initNotifier(false, false, false, subpool);

	svn_error_t *err = svn_client_status(&result_rev, svn_path_canonicalize( nurl.path().utf8(), subpool ), &rev, kio_svnProtocol::status, this, fullRecurse, getAll, checkRepos, no_ignore, ctx, subpool);

	if ( err )
		error( KIO::ERR_SLAVE_DEFINED, err->message );
	
	finished();
	svn_pool_destroy (subpool);
}

//change the proto and remove trailing /
//remove double / also 
QString kio_svnProtocol::makeSvnURL ( const KURL& url ) const {
	QString kproto = url.protocol();
	KURL tpURL = url;
	tpURL.cleanPath( true );
	QString svnUrl;
	if ( kproto == "svn+http" ) {
		kdDebug(7128) << "http:/ " << url.url() << endl;
		tpURL.setProtocol("http");
		svnUrl = tpURL.url(-1);
		return svnUrl;
	}
	else if ( kproto == "svn+https" ) {
		kdDebug(7128) << "https:/ " << url.url() << endl;
		tpURL.setProtocol("https");
		svnUrl = tpURL.url(-1);
		return svnUrl;
	}
	else if ( kproto == "svn+ssh" ) {
		kdDebug(7128) << "svn+ssh:/ " << url.url() << endl;
		tpURL.setProtocol("svn+ssh");
		svnUrl = tpURL.url(-1);
		return svnUrl;
	}
	else if ( kproto == "svn" ) {
		kdDebug(7128) << "svn:/ " << url.url() << endl;
		tpURL.setProtocol("svn");
		svnUrl = tpURL.url(-1);
		return svnUrl;
	}
	else if ( kproto == "svn+file" ) {
		kdDebug(7128) << "file:/ " << url.url() << endl;
		tpURL.setProtocol("file");
		svnUrl = tpURL.url(-1);
		//hack : add one more / after file:/
		int idx = svnUrl.find("/");
		svnUrl.insert( idx, "//" );
		return svnUrl;
	}
	return tpURL.url(-1);
}

QString kio_svnProtocol::chooseProtocol ( const QString& kproto ) const {
	if ( kproto == "svn+http" ) return QString( "http" );
	else if ( kproto == "svn+https" ) return QString( "https" );
	else if ( kproto == "svn+ssh" ) return QString( "svn+ssh" );
	else if ( kproto == "svn" ) return QString( "svn" );
	else if ( kproto == "svn+file" ) return QString( "file" );
	return kproto;
}

svn_error_t *kio_svnProtocol::trustSSLPrompt(svn_auth_cred_ssl_server_trust_t **cred_p, void *, const char */*realm*/, apr_uint32_t /*failures*/, const svn_auth_ssl_server_cert_info_t */*cert_info*/, svn_boolean_t /*may_save*/, apr_pool_t *pool) {
	//when ksvnd is ready make it prompt for the SSL certificate ... XXX
	*cred_p = (svn_auth_cred_ssl_server_trust_t*)apr_pcalloc (pool, sizeof (**cred_p));
	(*cred_p)->may_save = FALSE;
	return SVN_NO_ERROR;
}

svn_error_t *kio_svnProtocol::clientCertSSLPrompt(svn_auth_cred_ssl_client_cert_t **/*cred_p*/, void *, const char */*realm*/, svn_boolean_t /*may_save*/, apr_pool_t */*pool*/) {
	//when ksvnd is ready make it prompt for the SSL certificate ... XXX
/*	*cred_p = apr_palloc (pool, sizeof(**cred_p));
	(*cred_p)->cert_file = cert_file;*/
	return SVN_NO_ERROR;
}

svn_error_t *kio_svnProtocol::clientCertPasswdPrompt(svn_auth_cred_ssl_client_cert_pw_t **/*cred_p*/, void *, const char */*realm*/, svn_boolean_t /*may_save*/, apr_pool_t */*pool*/) {
	//when ksvnd is ready make it prompt for the SSL certificate password ... XXX
	return SVN_NO_ERROR;
}

svn_error_t *kio_svnProtocol::commitLogPrompt( const char **log_msg, const char **/*file*/, apr_array_header_t *commit_items, void *baton, apr_pool_t *pool ) {
	QCString replyType;
	QByteArray params;
	QByteArray reply;
	QString result;
	QStringList slist;
	kio_svnProtocol *p = ( kio_svnProtocol* )baton;
	svn_stringbuf_t *message = NULL;

	for (int i = 0; i < commit_items->nelts; i++) {
		QString list;
		svn_client_commit_item_t *item = ((svn_client_commit_item_t **) commit_items->elts)[i];
		const char *path = item->path;
		char text_mod = '_', prop_mod = ' ';

		if (! path)
			path = item->url;
		else if (! *path)
			path = ".";

		if (! path)
			path = ".";

		if ((item->state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE) && (item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD))
			text_mod = 'R';
		else if (item->state_flags & SVN_CLIENT_COMMIT_ITEM_ADD)
			text_mod = 'A';
		else if (item->state_flags & SVN_CLIENT_COMMIT_ITEM_DELETE)
			text_mod = 'D';
		else if (item->state_flags & SVN_CLIENT_COMMIT_ITEM_TEXT_MODS)
			text_mod = 'M';
		if (item->state_flags & SVN_CLIENT_COMMIT_ITEM_PROP_MODS)
			prop_mod = 'M';

		list += text_mod;
		list += " ";
		list += prop_mod;
		list += "  ";
		list += path;
		kdDebug(7128) << " Commiting items : " << list << endl;
		slist << list;
	}

	QDataStream stream(params, IO_WriteOnly);
	stream << slist.join("\n");	

	if ( !p->dcopClient()->call( "kded","ksvnd","commitDialog(QString)", params, replyType, reply ) ) {
		kdWarning() << "Communication with KDED:KSvnd failed" << endl;
		return SVN_NO_ERROR;
	}

	if ( replyType != "QString" ) {
		kdWarning() << "Unexpected reply type" << endl;
		return SVN_NO_ERROR;
	}
	
	QDataStream stream2 ( reply, IO_ReadOnly );
	stream2 >> result;
	
	if ( result.isNull() ) { //cancelled
		*log_msg = NULL;
		return SVN_NO_ERROR;
	}
		
	message = svn_stringbuf_create( result.utf8(), pool );
	*log_msg = message->data;

	return SVN_NO_ERROR;
}

void kio_svnProtocol::notify(void *baton, const char *path, svn_wc_notify_action_t action, svn_node_kind_t kind, const char *mime_type, svn_wc_notify_state_t content_state, svn_wc_notify_state_t prop_state, svn_revnum_t revision) {
	kdDebug(7128) << "NOTIFY : " << path << " updated at revision " << revision << " action : " << action << ", kind : " << kind << " , content_state : " << content_state << ", prop_state : " << prop_state << endl;

	QString userstring;
	struct notify_baton *nb = ( struct notify_baton* ) baton;

	//// Convert notification to a user readable string
	switch ( action ) {
		case svn_wc_notify_add : //add
			if (mime_type && (svn_mime_type_is_binary (mime_type)))
				userstring = i18n( "A (bin) %1" ).arg( path );
			else
				userstring = i18n( "A %1" ).arg( path );
			break;
		case svn_wc_notify_copy: //copy
			break;
		case svn_wc_notify_delete: //delete
			nb->received_some_change = TRUE;
			userstring = i18n( "D %1" ).arg( path );
			break;
		case svn_wc_notify_restore : //restore
			userstring=i18n( "Restored %1." ).arg( path );
			break;
		case svn_wc_notify_revert : //revert
			userstring=i18n( "Reverted %1." ).arg( path );
			break;
		case svn_wc_notify_failed_revert: //failed revert
			userstring=i18n( "Failed to revert %1.\nTry updating instead." ).arg( path );
			break;
		case svn_wc_notify_resolved: //resolved
			userstring=i18n( "Resolved conflicted state of %1." ).arg( path );
			break;
		case svn_wc_notify_skip: //skip
			if ( content_state == svn_wc_notify_state_missing )
				userstring=i18n("Skipped missing target %1.").arg( path );
			else 
				userstring=i18n("Skipped  %1.").arg( path );
			break;
		case svn_wc_notify_update_delete: //update_delete
			nb->received_some_change = TRUE;
			userstring=i18n( "D %1" ).arg( path );
			break;
		case svn_wc_notify_update_add: //update_add
			nb->received_some_change = TRUE;
			userstring=i18n( "A %1" ).arg( path );
			break;
		case svn_wc_notify_update_update: //update_update
			{
				/* If this is an inoperative dir change, do no notification.
				   An inoperative dir change is when a directory gets closed
				   without any props having been changed. */
				if (! ((kind == svn_node_dir)
							&& ((prop_state == svn_wc_notify_state_inapplicable)
								|| (prop_state == svn_wc_notify_state_unknown)
								|| (prop_state == svn_wc_notify_state_unchanged)))) {
					nb->received_some_change = TRUE;

					if (kind == svn_node_file) {
						if (content_state == svn_wc_notify_state_conflicted)
							userstring = "C";
						else if (content_state == svn_wc_notify_state_merged)
							userstring = "G";
						else if (content_state == svn_wc_notify_state_changed)
							userstring = "U";
					}

					if (prop_state == svn_wc_notify_state_conflicted)
						userstring += "C";
					else if (prop_state == svn_wc_notify_state_merged)
						userstring += "G";
					else if (prop_state == svn_wc_notify_state_changed)
						userstring += "U";
					else
						userstring += " ";

					if (! ((content_state == svn_wc_notify_state_unchanged
									|| content_state == svn_wc_notify_state_unknown)
								&& (prop_state == svn_wc_notify_state_unchanged
									|| prop_state == svn_wc_notify_state_unknown)))
						userstring += QString( " " ) + path;
				}
				break;
			}
		case svn_wc_notify_update_completed: //update_completed
			{
				if (! nb->suppress_final_line) {
					if (SVN_IS_VALID_REVNUM (revision)) {
						if (nb->is_export) {
							if ( nb->in_external ) 
								userstring = i18n("Exported external at revision %1.").arg( revision );
							else 
								userstring = i18n("Exported revision %1.").arg( revision );
						} else if (nb->is_checkout) {
							if ( nb->in_external )
								userstring = i18n("Checked out external at revision %1.").arg( revision );
							else
								userstring = i18n("Checked out revision %1.").arg( revision);
						} else {
							if (nb->received_some_change) {
								if ( nb->in_external )
									userstring=i18n("Updated external to revision %1.").arg( revision );
								else 
									userstring = i18n("Updated to revision %1.").arg( revision);
							} else {
								if ( nb->in_external )
									userstring = i18n("External at revision %1.").arg( revision );
								else
									userstring = i18n("At revision %1.").arg( revision);
							}
						}
					} else  /* no revision */ {
						if (nb->is_export) {
							if ( nb->in_external )
								userstring = i18n("External export complete.");
							else
								userstring = i18n("Export complete.");
						} else if (nb->is_checkout) {
							if ( nb->in_external )
								userstring = i18n("External checkout complete.");
							else
								userstring = i18n("Checkout complete.");
						} else {
							if ( nb->in_external )
								userstring = i18n("External update complete.");
							else
								userstring = i18n("Update complete.");
						}
					}
				}
			}
			if (nb->in_external)
				nb->in_external = FALSE;
			break;
		case svn_wc_notify_update_external: //update_external
			nb->in_external = TRUE;
			userstring = i18n("Fetching external item into %1." ).arg( path );
			break;
		case svn_wc_notify_status_completed: //status_completed
			if (SVN_IS_VALID_REVNUM (revision))
				userstring = i18n( "Status against revision: %1.").arg( revision );
			break;
		case svn_wc_notify_status_external: //status_external
             userstring = i18n("Performing status on external item at %1.").arg( path ); 
			break;
		case svn_wc_notify_commit_modified: //commit_modified
			userstring = i18n( "Sending %1").arg( path );
			break;
		case svn_wc_notify_commit_added: //commit_added
			if (mime_type && svn_mime_type_is_binary (mime_type)) {
				userstring = i18n( "Adding (bin) %1.").arg( path );
			} else {
				userstring = i18n( "Adding %1.").arg( path );
			}
			break;
		case svn_wc_notify_commit_deleted: //commit_deleted
			userstring = i18n( "Deleting %1.").arg( path );
			break; 
		case svn_wc_notify_commit_replaced: //commit_replaced
			userstring = i18n( "Replacing %1.").arg( path );
			break;
		case svn_wc_notify_commit_postfix_txdelta: //commit_postfix_txdelta
			if (! nb->sent_first_txdelta) {
				nb->sent_first_txdelta = TRUE;
				userstring=i18n("Transmitting file data ");
			} else {
				userstring=".";
			}
			break;

			break;
		case svn_wc_notify_blame_revision: //blame_revision
			break;
		default:
			break;
	}
	//// End convert

	kio_svnProtocol *p = ( kio_svnProtocol* )nb->master;

	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "path" , QString::fromUtf8( path ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "action", QString::number( action ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "kind", QString::number( kind ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "mime_t", QString::fromUtf8( mime_type ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "content", QString::number( content_state ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "prop", QString::number( prop_state ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "rev", QString::number( revision ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "string", userstring );
	p->incCounter();
}

void kio_svnProtocol::status(void *baton, const char *path, svn_wc_status_t *status) {
	kdDebug(7128) << "STATUS : " << path << ", wc text status : " << status->text_status 
									 << ", wc prop status : " << status->prop_status
									 << ", repos text status : " << status->repos_text_status
									 << ", repos prop status : " << status->repos_prop_status 
									 << endl;

	QByteArray params;
	kio_svnProtocol *p = ( kio_svnProtocol* )baton;

	QDataStream stream(params, IO_WriteOnly);
	long int rev = status->entry ? status->entry->revision : 0;
	stream << QString::fromUtf8( path ) << status->text_status << status->prop_status << status->repos_text_status << status->repos_prop_status << rev;

	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "path", QString::fromUtf8( path ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "text", QString::number( status->text_status ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "prop", QString::number( status->prop_status ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "reptxt", QString::number( status->repos_text_status ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "repprop", QString::number( status->repos_prop_status ));
	p->setMetaData(QString::number( p->counter() ).rightJustify( 10,'0' )+ "rev", QString::number( rev ));
	p->incCounter();
}


void kio_svnProtocol::wc_resolve( const KURL& wc, bool recurse ) {
	kdDebug(7128) << "kio_svnProtocol::wc_resolve() : " << wc.url() << endl;
	
	apr_pool_t *subpool = svn_pool_create (pool);

	KURL nurl = wc;
	nurl.setProtocol( "file" );
	recordCurrentURL( nurl );

	initNotifier(false, false, false, subpool);
	svn_error_t *err = svn_client_resolved(svn_path_canonicalize( nurl.path().utf8(), subpool ), recurse,ctx,subpool);
	if ( err )
		error( KIO::ERR_SLAVE_DEFINED, err->message );
	
	finished();
	svn_pool_destroy (subpool);
}

extern "C"
{
	KDE_EXPORT int kdemain(int argc, char **argv)    {
		KInstance instance( "kio_svn" );

		kdDebug(7128) << "*** Starting kio_svn " << endl;

		if (argc != 4) {
			kdDebug(7128) << "Usage: kio_svn  protocol domain-socket1 domain-socket2" << endl;
			exit(-1);
		}

		kio_svnProtocol slave(argv[2], argv[3]);
		slave.dispatchLoop();

		kdDebug(7128) << "*** kio_svn Done" << endl;
		return 0;
	}
}

