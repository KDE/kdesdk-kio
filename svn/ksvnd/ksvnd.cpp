/*
    This file is part of the KDE Project

    Copyright (C) 2003, 2004 Mickael Marchand <marchand@kde.org>

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This software is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this library; see the file COPYING. If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include <klocale.h>
#include <kdebug.h>
#include <kmessagebox.h>
#include <qdir.h>
#include <qfile.h>
#include <QTextStream>
#include "ksvndadaptor.h"

#include "ksvnd.h"

#include <kpluginfactory.h>
#include <kpluginloader.h>

K_PLUGIN_FACTORY(KSvndFactory,
                 registerPlugin<KSvnd>();
    )
K_EXPORT_PLUGIN(KSvndFactory("kio_svn"))

KSvnd::KSvnd(QObject* parent, const QList<QVariant>&)
 : KDEDModule(parent) {
   new KsvndAdaptor(this);
}

KSvnd::~KSvnd() {
}

QString KSvnd::commitDialog(const QString &modifiedFiles) {
	CommitDlg commitDlg(0);
	commitDlg.setLog( modifiedFiles );
	int result = commitDlg.exec();
	if ( result == QDialog::Accepted ) {
		return commitDlg.logMessage();
	} else
		return QString();
}

bool KSvnd::AreAnyFilesInSvn( const QStringList& lst ) {
	KUrl::List wclist(lst);
	for ( QList<KUrl>::const_iterator it = wclist.constBegin(); it != wclist.constEnd() ; ++it ) {
		kDebug( 7128 ) << "Checking file " << ( *it );
		QDir bdir ( ( *it ).toLocalFile() );
		if ( bdir.exists() && QFile::exists( ( *it ).toLocalFile() + "/.svn/entries" ) ) {
			return true;
		} else if ( !bdir.exists() ) {
			if ( isFileInSvnEntries( ( *it ).fileName(), ( *it ).directory() + "/.svn/entries" ) || isFileInExternals ( ( *it ).fileName(), ( *it ).directory()+"/.svn/dir-props" ) )
				return true;
		}
	}
	return false;
}

bool KSvnd::AreAnyFilesNotInSvn( const QStringList& lst ) {
	KUrl::List wclist(lst);
	for ( QList<KUrl>::const_iterator it = wclist.constBegin(); it != wclist.constEnd() ; ++it ) {
		kDebug( 7128 ) << "Checking file " << ( *it );
		QDir bdir ( ( *it ).toLocalFile() );
		if ( bdir.exists() && !QFile::exists( ( *it ).toLocalFile() + "/.svn/entries" ) ) {
			return true;
		} else if ( !bdir.exists() ) {
			if ( !isFileInSvnEntries( ( *it ).fileName(),( *it ).directory() + "/.svn/entries" ) && !isFileInExternals ( ( *it ).fileName(), ( *it ).directory()+"/.svn/dir-props" ) )
				return true;
		}
	}
	return false;
}

bool KSvnd::AreAllFilesInSvn( const QStringList& lst ) {
	KUrl::List wclist(lst);
	for ( QList<KUrl>::const_iterator it = wclist.constBegin(); it != wclist.constEnd() ; ++it ) {
		kDebug( 7128 ) << "Checking file " << ( *it );
		QDir bdir ( ( *it ).toLocalFile() );
		if ( bdir.exists() && !QFile::exists( ( *it ).toLocalFile() + "/.svn/entries" ) ) {
			return false;
		} else if ( !bdir.exists() ) {
			if ( !isFileInSvnEntries( ( *it ).fileName(),( *it ).directory() + "/.svn/entries" ) && !isFileInExternals ( ( *it ).fileName(), ( *it ).directory()+"/.svn/dir-props" )  )
				return false;
		}
	}
	return true;
}

bool KSvnd::AreAllFilesNotInSvn( const QStringList& lst ) {
	KUrl::List wclist(lst);
	for ( QList<KUrl>::const_iterator it = wclist.constBegin(); it != wclist.constEnd() ; ++it ) {
		kDebug( 7128 ) << "Checking file " << ( *it );
		QDir bdir ( ( *it ).toLocalFile() );
		if ( bdir.exists() && QFile::exists( ( *it ).toLocalFile() + "/.svn/entries" ) ) {
			return false;
		} else if ( !bdir.exists() ) {
			if ( isFileInSvnEntries( ( *it ).fileName(),( *it ).directory() + "/.svn/entries" ) || isFileInExternals ( ( *it ).fileName(), ( *it ).directory()+"/.svn/dir-props" ) )
				return false;
		}
	}
	return true;
}

bool KSvnd::isFileInSvnEntries ( const QString &filename, const QString &entfile ) {
	QFile file( entfile );
	bool potential = false;
	if ( file.open( QIODevice::ReadOnly ) ) {
		QTextStream stream( &file );
		QString line;
		while ( !stream.atEnd() ) {
			line = stream.readLine().simplified();

			if ( potential == true ) {
			// Check that filename is really a file or dir
				if ( line == "dir" || line == "file" ) {
					file.close();
					return true;
				} else {
			// Reset potential to false
				potential=false;
				}
			}

			if ( line == filename ) {
			// Assume we're using SVN >= 1.4
				potential=true;
			} else if ( line == "name=\""+ filename + "\"" ) {
			// We could still be using SVN <= 1.3 (XML format)
				file.close();
				return true;
			}
		}
		file.close();
	}
	return false;
}

bool KSvnd::isFileInExternals ( const QString &filename, const QString &propfile ) {
	QFile file( propfile );
	if ( file.open( QIODevice::ReadOnly ) ) {
		QTextStream stream( &file );
		QStringList line;
		while ( !stream.atEnd() )
			line << stream.readLine().simplified();
		for ( int i = 0 ; i < line.count(); i++ ) {
			if ( line[ i ] == "K 13"  && line[ i+1 ] == "svn:externals" ) { //Key 13 : svn:externals
				//next line should be "V xx"
				if ( line [ i+2 ].startsWith( "V " ) ) {
					//ok browse the values now
					i+=2;
					while ( i < line.count() ) {
						if ( line[ i ].startsWith( filename+" " ) ) { //found it !
							file.close( );
							return true;
						} else if ( line[ i ].isEmpty() ) {
							file.close( );
							return false; //we are out of svn:externals now...
						}
						i++;
					}
				}
			}
		}
		file.close();
	}
	return false;
}

bool KSvnd::anyNotValidWorkingCopy( const QStringList& lst ) {
	KUrl::List wclist(lst);
	bool result = true; //one negative match is enough
	for ( QList<KUrl>::const_iterator it = wclist.constBegin(); it != wclist.constEnd() ; ++it ) {
		//exception for .svn dirs
		if ( ( *it ).path(KUrl::RemoveTrailingSlash).endsWith( "/.svn" ) )
			return true;
		//if is a directory check whether it contains a .svn/entries file
		QDir dir( ( *it ).toLocalFile() );
		if ( dir.exists() ) { //it's a dir
			if ( !QFile::exists( ( *it ).toLocalFile() + "/.svn/entries" ) )
				result = false;
		}

		//else check if ./.svn/entries exists
		if ( !QFile::exists( ( *it ).directory() + "/.svn/entries" ) )
			result = false;
	}
	return result;
}

bool KSvnd::anyValidWorkingCopy( const QStringList &lst ) {
	KUrl::List wclist(lst);
	for ( QList<KUrl>::const_iterator it = wclist.constBegin(); it != wclist.constEnd() ; ++it ) {
		//skip .svn dirs
		if ( ( *it ).path(KUrl::RemoveTrailingSlash).endsWith( "/.svn" ) )
			continue;
		//if is a directory check whether it contains a .svn/entries file
		QDir dir( ( *it ).toLocalFile() );
		if ( dir.exists() ) { //it's a dir
			if ( QFile::exists( ( *it ).toLocalFile() + "/.svn/entries" ) )
				return true;
		}

		//else check if ./.svn/entries exists
		if ( QFile::exists( ( *it ).directory() + "/.svn/entries" ) )
			return true;
	}
	return false;
}

int KSvnd::getStatus( const KUrl::List& list ) {
	int result = 0;
	int files = 0, folders = 0, parentsentries = 0, parentshavesvn = 0, subdirhavesvn = 0, external = 0;
	for ( QList<KUrl>::const_iterator it = list.begin(); it != list.end() ; ++it ) {
		if ( isFolder ( ( *it ) ) ) {
			folders++;
		} else {
			files++;
		}
		if ( isFileInSvnEntries ( (*it).fileName(),( *it ).directory() + "/.svn/entries" ) ) { // normal subdir known in the working copy
			parentsentries++;
		} else if ( isFolder( *it ) ) { // other subfolders (either another module checkouted or an external, or something not known at all)
			if ( QFile::exists( ( *it ).toLocalFile() + "/.svn/entries" ) )
				subdirhavesvn++;
			if ( isFileInExternals( (*it).fileName(), ( *it ).directory() + "/.svn/dir-props" ) ) {
				external++;
			}
		}
		if ( ( isFolder( ( *it ) ) && QFile::exists( ( *it ).directory() + "../.svn/entries" ) ) || QFile::exists( ( *it ).directory() + "/.svn/entries" ) ) //parent has a .svn ?
			parentshavesvn++;
	}
	if ( files > 0 )
		result |= SomeAreFiles;
	if ( folders == list.count() ) {
		result |= AllAreFolders;
		result |= SomeAreFolders;
	}
	if ( folders > 0 )
		result |= SomeAreFolders;
	if ( parentsentries == list.count() ) {
		result |= AllAreInParentsEntries;
		result |= SomeAreInParentsEntries;
	} else if ( parentsentries != 0 )
		result |= SomeAreInParentsEntries;
	if ( parentshavesvn == list.count() ) {
		result |= AllParentsHaveSvn;
		result |= SomeParentsHaveSvn;
	} else if ( parentshavesvn > 0 )
		result |= SomeParentsHaveSvn;
	if ( subdirhavesvn == list.count() ) {
		result |= AllHaveSvn;
		result |= SomeHaveSvn;
	} else if ( subdirhavesvn > 0 )
		result |= SomeHaveSvn;
	if ( external == list.count() ) {
		result |= AllAreExternalToParent;
		result |= SomeAreExternalToParent;
	} else if ( external > 0 )
		result |= SomeAreExternalToParent;

	return result;
}

bool KSvnd::isFolder( const KUrl& url ) {
	QDir d( url.toLocalFile() );
	return d.exists();
}

QStringList KSvnd::getActionMenu ( const QStringList &lst ) {
	KUrl::List list(lst);
	QStringList result;
	int listStatus = getStatus( list );

	if ( !(listStatus & SomeAreInParentsEntries) &&
	     !(listStatus & SomeAreExternalToParent) &&
	     !(listStatus & SomeHaveSvn)) {
		if ( (listStatus & AllParentsHaveSvn) ) {
			// These files can only be added to SVN
			result << "Add";
			result << "_SEPARATOR_";
		}

		if( list.size() == 1 && listStatus & SomeAreFolders) {
			result << "Checkout";
			result << "Export";
//			result << "CreateRepository";
			result << "Import";
		}
	} else if ( (listStatus & AllAreInParentsEntries) ) {
		result << "Diff";
		//In SVN
//		result << "ShowLog";
//		result << "CheckForModifications";
//		result << "RevisionGraph";
//		result << "_SEPARATOR_";
//		result << "Update to revision..."
		result << "Rename";
		result << "Delete";
		if( listStatus & SomeAreFolders && !(listStatus & SomeAreFiles)) {
			result << "Revert";
//			result << "Cleanup";
		}
		result << "_SEPARATOR_";
//		result << "BranchTag";
		result << "Switch";
		result << "Merge";
		if( listStatus & SomeAreFolders && !(listStatus & SomeAreFiles)) {
//			result << "Export";
//			result << "Relocate";
			result << "_SEPARATOR_";
			result << "Add";
		}
		result << "_SEPARATOR_";
		if( listStatus & SomeAreFiles && !(listStatus & SomeAreFolders)) {
			result << "Blame";
		}
		result << "CreatePatch";

		if( list.size() == 1 && listStatus & SomeAreFolders) {
//			result << "ApplyPatchToFolder";
		}
	}
	return result;
}

QStringList KSvnd::getTopLevelActionMenu ( const QStringList &lst ) {
	KUrl::List wclist(lst);
	QStringList result;
	int listStatus = getStatus( lst );


	if ( ( listStatus & AllParentsHaveSvn &&
			( ( listStatus & SomeAreExternalToParent ) || ( listStatus & SomeAreInParentsEntries ) )
				|| ( listStatus & SomeHaveSvn ) )
		) {
		result << "Update";
		result << "Commit";
	}

	return result;
}

#if 0
void KSvnd::notify(const QString& path, int action, int kind, const QString& mime_type, int content_state, int prop_state, long int revision, const QString& userstring) {
	kDebug(7128) << "KDED/Subversion : notify " << path << " action : " << action << " mime_type : " << mime_type << " content_state : " << content_state << " prop_state : " << prop_state << " revision : " << revision << " userstring : " << userstring;
	QByteArray params;

	QDataStream stream( &params,QIODevice::WriteOnly);
	stream << path << action << kind << mime_type << content_state << prop_state << revision << userstring;

	emitDCOPSignal( "subversionNotify(QString,int,int,QString,int,int,long int,QString)", params );
}

void KSvnd::status(const QString& path, int text_status, int prop_status, int repos_text_status, int repos_prop_status, long int rev ) {
	kDebug(7128) << "KDED/Subversion : status " << path << " " << text_status << " " << prop_status << " "
			<< repos_text_status << " " << repos_prop_status << " " << rev << endl;
	QByteArray params;

	QDataStream stream( &params,QIODevice::WriteOnly);
	stream << path << text_status << prop_status << repos_text_status << repos_prop_status << rev;

	emitDCOPSignal( "subversionStatus(QString,int,int,int,int,long int)", params );
}
#endif

void KSvnd::popupMessage( const QString& message ) {
	kDebug(7128) << "KDED/Subversion : popupMessage" << message;
	KMessageBox::information(0, message, i18n( "Subversion" ) );
}

#include "ksvnd.moc"
