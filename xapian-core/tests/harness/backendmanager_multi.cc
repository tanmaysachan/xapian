/** @file backendmanager_multi.cc
 * @brief BackendManager subclass for multi databases.
 */
/* Copyright (C) 2007,2008,2009,2011,2012,2013,2015,2017,2018,2019 Olly Betts
 * Copyright (C) 2008 Lemur Consulting Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <config.h>

#include "backendmanager_multi.h"
#include "backendmanager_remote.h"

#include "errno_to_string.h"
#include "filetests.h"
#include "index_utils.h"
#include "str.h"

#include <cerrno>
#include <cstdio> // For rename().
#include <cstring>

#ifdef HAVE_VALGRIND
# include <valgrind/memcheck.h>
#endif

using namespace std;

BackendManagerMulti::BackendManagerMulti(const std::string& datadir_,
					 vector<BackendManager*> sub_managers_)
    : BackendManager(datadir_),
      sub_managers(sub_managers_)
{
    cachedir = ".multi";
    if (sub_managers.size() == 2 &&
	sub_managers[0]->get_dbtype() == sub_managers[1]->get_dbtype()) {
	cachedir += sub_managers[0]->get_dbtype();
    } else {
	for (auto sub_manager: sub_managers) {
	    cachedir += sub_manager->get_dbtype();
	}
    }
    // Ensure the directory we store cached test databases in exists.
    (void)create_dir_if_needed(cachedir);
}

std::string
BackendManagerMulti::get_dbtype() const
{
    string dbtype = "multi";
    if (sub_managers.size() == 2 &&
	sub_managers[0]->get_dbtype() == sub_managers[1]->get_dbtype()) {
	dbtype += "_" + sub_managers[0]->get_dbtype();
    } else {
	for (auto sub_manager: sub_managers) {
	    dbtype += "_" + sub_manager->get_dbtype();
	}
    }
    return dbtype;
}

#define NUMBER_OF_SUB_DBS 2

string
BackendManagerMulti::createdb_multi(const string& name,
				    const vector<string>& files)
{
    string dbname;
    if (!name.empty()) {
	dbname = name;
    } else {
	dbname = "db";
	for (const string& file : files) {
	    dbname += "__";
	    dbname += file;
	}
    }

    string db_path = cachedir;
    db_path += '/';
    db_path += dbname;

    if (!name.empty()) {
	remove(db_path.c_str());
    } else {
	if (file_exists(db_path)) return db_path;
    }

    string tmpfile = db_path + ".tmp";
    ofstream out(tmpfile.c_str());
    if (!out.is_open()) {
	string msg = "Couldn't create file '";
	msg += tmpfile;
	msg += "' (";
	errno_to_string(errno, msg);
	msg += ')';
	throw msg;
    }

    // Open NUMBER_OF_SUB_DBS databases and index files to them alternately so
    // a multi-db combining them contains the documents in the expected order.
    Xapian::WritableDatabase dbs;

    string dbbase = db_path;
    dbbase += "___";
    size_t dbbase_len = dbbase.size();

    for (size_t n = 0; n < NUMBER_OF_SUB_DBS; ++n) {
	const string &subtype = sub_managers[n]->get_dbtype();
	int flags = Xapian::DB_CREATE_OR_OVERWRITE;
	if (subtype == "glass") {
	    flags |= Xapian::DB_BACKEND_GLASS;
	    dbbase += str(n);
	    dbs.add_database(Xapian::WritableDatabase(dbbase, flags));
	    string line = subtype + " " + dbname + "___" + str(n);
	    out << line << '\n';
	} else if (subtype == "remoteprog_glass") {
	    flags |= Xapian::DB_BACKEND_GLASS;
	    dbbase += str(n);
	    Xapian::WritableDatabase remote_db(dbbase, flags);
	    remote_db.close();
	    string args = sub_managers[n]->get_writable_database_args(dbbase, 300000);
#ifdef HAVE_VALGRIND
	    if (RUNNING_ON_VALGRIND) {
		args.insert(0, XAPIAN_PROGSRV" ");
		dbs.add_database(Xapian::Remote::open_writable("./runsrv", args));
		out << "remote :" << args << '\n';
	    }
#else
	    dbs.add_database(Xapian::Remote::open_writable(XAPIAN_PROGSRV, args));
	    out << "remote :" << XAPIAN_PROGSRV << " " << args << '\n';
#endif
	} else {
	    string msg = "Unknown multidb subtype: ";
	    msg += subtype;
	    throw msg;
	}
	dbbase.resize(dbbase_len);
    }

    out.close();

    FileIndexer(get_datadir(), files).index_to(dbs);
    dbs.close();

    if (rename(tmpfile.c_str(), db_path.c_str()) < 0) {
	throw Xapian::DatabaseError("rename failed", errno);
    }

    last_wdb_path = db_path;
    return db_path;
}

string
BackendManagerMulti::do_get_database_path(const vector<string> & files)
{
    return createdb_multi(string(), files);
}

Xapian::WritableDatabase
BackendManagerMulti::get_writable_database(const string& name, const string& file)
{
    vector<string> files;
    if (!file.empty()) files.push_back(file);
    return Xapian::WritableDatabase(createdb_multi(name, files));
}

string
BackendManagerMulti::get_writable_database_path(const std::string& name)
{
    return cachedir + "/" + name;
}

string
BackendManagerMulti::get_compaction_output_path(const string& name)
{
    return cachedir + "/" + name;
}

string
BackendManagerMulti::get_generated_database_path(const string& name)
{
    return BackendManagerMulti::get_writable_database_path(name);
}

Xapian::WritableDatabase
BackendManagerMulti::get_writable_database_again()
{
    return Xapian::WritableDatabase(last_wdb_path);
}

string
BackendManagerMulti::get_writable_database_path_again()
{
    return last_wdb_path;
}
