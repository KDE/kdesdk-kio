/*
    This file is part of the KDE Project

    Copyright (C) 2003-2005 Mickael Marchand <marchand@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    version 2 as published by the Free Software Foundation.

    This software is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this library; see the file COPYING. If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#ifndef KSVND_H
#define KSVND_H

#include <kdedmodule.h>
#include <kurl.h>
#include <qstringlist.h>
#include <QByteArray>

class KSvnd : public KDEDModule
{
  Q_OBJECT
      
  //note: InSVN means parent is added.  InRepos  means itself is added
  enum { SomeAreFiles = 1, SomeAreFolders = 2,  SomeAreInParentsEntries = 4, SomeParentsHaveSvn = 8, SomeHaveSvn = 16, SomeAreExternalToParent = 32, AllAreInParentsEntries = 64, AllParentsHaveSvn = 128, AllHaveSvn = 256, AllAreExternalToParent = 512, AllAreFolders = 1024 };
public:
  KSvnd();
  ~KSvnd();

  public Q_SLOTS: //dbus function for me KUrl::List must be changed
//  void addAuthInfo(KIO::AuthInfo, long);
  QString commitDialog(QString);
  bool anyNotValidWorkingCopy( const KUrl::List& wclist );
  bool anyValidWorkingCopy( const KUrl::List& wclist );
  bool AreAnyFilesNotInSvn( const KUrl::List& wclist );
  bool AreAnyFilesInSvn( const KUrl::List& wclist );
  bool AreAllFilesNotInSvn( const KUrl::List& wclist );
  bool AreAllFilesInSvn( const KUrl::List& wclist );
  QStringList getActionMenu ( const KUrl::List& list );
  QStringList getTopLevelActionMenu ( const KUrl::List &list );

public slots:

protected:
  bool isFileInSvnEntries ( const QString filename, const QString entfile );
  bool isFileInExternals ( const QString filename, const QString propfile );
  bool isFolder( const KUrl& url );
  int getStatus( const KUrl::List& list );
};

#endif
