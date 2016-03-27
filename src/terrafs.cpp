//============================================================================
// Name        : terrafs.cpp
// Author      : Torsten Dreyer - torsten(at)t3r(dot)de
// Copyright   : (c) Torsten Dreyer 2016
// Description : A fuse file system for FlightGear TerraScenery
//============================================================================
/*
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
#include <iostream>
#include <string>
#include <memory>
#include <sstream>
#include <vector>
#include <map>
#include <stdlib.h>
#include <curl/multi.h>
#include <string.h>
#include <stddef.h>

#define FUSE_USE_VERSION 30
#define _FILE_OFFSET_BITS 64
#include <fuse.h>

using namespace std;

vector<string> &split(const string &s, char delim, vector<string> &elems)
{
	stringstream ss(s);
	string item;
	while (getline(ss, item, delim))
	{
		elems.push_back(item);
	}
	return elems;
}

vector<string> split(const string &s, char delim)
{
	vector<string> elems;
	split(s, delim, elems);
	return elems;
}

class DirIndexEntry
{
public:
	DirIndexEntry(const vector<string> & data);
	virtual ~DirIndexEntry();
	string getName() const
	{
		return name;
	}
private:
	string name;
};

typedef shared_ptr<DirIndexEntry> DirIndexEntry_ptr;
typedef vector<DirIndexEntry_ptr> DirIndexEntryList;

DirIndexEntry::DirIndexEntry(const vector<string> & data) :
		name(data[1])
{
}

DirIndexEntry::~DirIndexEntry()
{
}

class FileDirIndexEntry: public DirIndexEntry
{
public:
	FileDirIndexEntry(const vector<string> & data);
	virtual ~FileDirIndexEntry();
	unsigned getSize() const
	{
		return size;
	}
private:
	unsigned size;
};

FileDirIndexEntry::FileDirIndexEntry(const vector<string> & data) :
		DirIndexEntry(data), size(stol(data[3]))
{
}

FileDirIndexEntry::~FileDirIndexEntry()
{
}

class DirDirIndexEntry: public DirIndexEntry
{
public:
	DirDirIndexEntry(const vector<string> & data);
	virtual ~DirDirIndexEntry();
private:
};

DirDirIndexEntry::DirDirIndexEntry(const vector<string> & data) :
		DirIndexEntry(data)

{

}

DirDirIndexEntry::~DirDirIndexEntry()
{
}

class DirIndex
{
public:
	DirIndex(const string & data);
	virtual ~DirIndex();
	const DirIndexEntryList getEntries() const
	{
		return entries;
	}

	const DirIndexEntry_ptr find(const string & name);
private:
	unsigned version;
	string path;
	DirIndexEntryList entries;
};

typedef shared_ptr<DirIndex> DirIndex_ptr;
typedef map<const string, DirIndex_ptr> DirIndexCache;

DirIndex::DirIndex(const string & data)
{

	stringstream ss(data);
	string line;

	while (getline(ss, line))
	{
		vector<string> tokens = split(line, ':');

		if (tokens[0] == "version")
		{
			version = stol(tokens[1]);
		}
		else if (tokens[0] == "path")
		{
			path = tokens.size() > 2 ? tokens[1] : "";
		}
		else if (tokens[0] == "d")
		{
			entries.push_back(
					shared_ptr<DirIndexEntry>(new DirDirIndexEntry(tokens)));
		}
		else if (tokens[0] == "f")
		{
			entries.push_back(
					shared_ptr<DirIndexEntry>(new FileDirIndexEntry(tokens)));
		}
	}
}

DirIndex::~DirIndex()
{
}

const DirIndexEntry_ptr DirIndex::find(const string & name)
{
	for (DirIndexEntryList::const_iterator it = entries.begin();
			it != entries.end(); ++it)
	{
		if ((*it)->getName() == name)
			return *it;
	}
	return DirIndexEntry_ptr(NULL);
}

class Curlie
{
public:
	Curlie();
	~Curlie();

	unsigned getFile(const string & url, string & content);
private:
	static const unsigned MAX_CONNECTIONS = 2;

	CURLM * cm;
};

Curlie::Curlie()
{
	curl_global_init(CURL_GLOBAL_ALL);
	cm = curl_multi_init();
	curl_multi_setopt(cm, CURLMOPT_MAXCONNECTS, (long )MAX_CONNECTIONS);
}

Curlie::~Curlie()
{
	curl_multi_cleanup(cm);
	curl_global_cleanup();
}

size_t get_directory_write_callback(char *ptr, size_t size, size_t nmemb,
		void *userdata)
{
	string * ud = static_cast<string*>(userdata);
	size_t n = size * nmemb;
	ud->append(ptr, n);
	return n;
}

class AutoCurlEasyCleanup
{
public:
	AutoCurlEasyCleanup(CURL * curl) :
			_curl(curl)
	{
	}
	~AutoCurlEasyCleanup()
	{
		curl_easy_cleanup(_curl);
	}
private:
	CURL * _curl;
};

unsigned Curlie::getFile(const string & url, string & content)
{
	CURL * curl = curl_easy_init();
	if (!curl)
		return -1;

	AutoCurlEasyCleanup c(curl);

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, get_directory_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &content);

	CURLcode res;
	if ((res = curl_easy_perform(curl)) != CURLE_OK)
	{
		return -1;
	}

	long responseCode = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
	return responseCode;
}

class TerraFs
{
public:
	TerraFs(const char * baseUrl, bool staticRoot);
	int readDir(const string & path, void *buf, fuse_fill_dir_t filler,
			off_t offset, struct fuse_file_info *fi);

	int getAttr(const string &path, struct stat *stbuf);
	int open(const string & path, struct fuse_file_info * fi);
	int release(const string & path, struct fuse_file_info * fi);
	int read(const string & path, char * buf, size_t size, off_t offset,
			struct fuse_file_info * fi);

	DirIndexEntry_ptr getDirIndexEntry(const string & path);

	int test();
private:

	DirIndex_ptr getDirIndex(const string & path);
	string baseUrl;
	bool staticRoot;
	Curlie curlie;

	DirIndexCache dirIndexCache;
};

TerraFs::TerraFs(const char * base, bool stRoot ) :
		baseUrl(base ? base : "http://flightgear.sourceforge.net/scenery"),
		staticRoot(stRoot)
{
}

DirIndex_ptr TerraFs::getDirIndex(const string & path)
{
	string url = baseUrl + path;

	DirIndexCache::const_iterator it = dirIndexCache.find(url);
	if (it != dirIndexCache.end())
		return it->second;

	string dirindexContent;
	unsigned res = curlie.getFile(url + "/.dirindex", dirindexContent);

	// cache negative hits as NULL
	DirIndex_ptr di(res == 200 ? new DirIndex(dirindexContent) : NULL);
	dirIndexCache[url] = di;
	return di;
}

int TerraFs::readDir(const string & path, void *buf, fuse_fill_dir_t filler,
		off_t, struct fuse_file_info *)
{
	if (staticRoot && path == "/")
	{
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_nlink = 1;
		st.st_mode = S_IFDIR | 0544;
		filler(buf, "Airports", &st, 0);
		filler(buf, "Objects", &st, 0);
		filler(buf, "Models", &st, 0);
		filler(buf, "Terrain", &st, 0);
		return 0;
	}

	DirIndex_ptr dirIndex = getDirIndex(path);
	if ( NULL == dirIndex)
		return -ENOENT;

	const DirIndexEntryList entries = dirIndex->getEntries();
	for (DirIndexEntryList::const_iterator it = entries.begin();
			it != entries.end(); ++it)
	{
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_nlink = 1;
		FileDirIndexEntry * f = dynamic_cast<FileDirIndexEntry*>((*it).get());
		if (f)
		{
			st.st_mode = S_IFDIR | 0444;
			st.st_size = f->getSize();
		}

		DirDirIndexEntry * d = dynamic_cast<DirDirIndexEntry*>((*it).get());
		if (d)
		{
			st.st_mode = S_IFDIR | 544;
		}
		filler(buf, (*it)->getName().c_str(), &st, 0);
	}

	return 0;
}

DirIndexEntry_ptr TerraFs::getDirIndexEntry(const string & path)
{
	// strip last element to get parent dir
	size_t last = path.find_last_of('/');
	if (last == string::npos)
		return NULL;

	string parent = path.substr(0, last);
	string file = path.substr(last + 1);
//	if (parent.empty())
//		parent = "/";

	DirIndex_ptr dirIndex = getDirIndex(parent);
	if ( NULL == dirIndex)
		return NULL;

	return dirIndex->find(file);
}

int TerraFs::getAttr(const string &path, struct stat *stbuf)
{
	memset(stbuf, 0, sizeof(struct stat));
	if (path == "/")
	{
		stbuf->st_mode = S_IFDIR | 0544;
		stbuf->st_nlink = 1;
		return 0;
	}
	if (staticRoot)
	{
		if (path == "/Airports" || path == "/Objects" || path == "/Models"
				|| path == "/Terrain")
		{
			stbuf->st_mode = S_IFDIR | 0544;
			stbuf->st_nlink = 1;
			return 0;
		}
	}

	DirIndexEntry_ptr entry = getDirIndexEntry(path);
	if (entry == NULL)
		return -ENOENT;

	stbuf->st_nlink = 1;

	FileDirIndexEntry * f = dynamic_cast<FileDirIndexEntry*>(entry.get());
	if (f)
	{
		// it's a file!
		stbuf->st_mode = S_IFREG | 0444; // r--r--r--
		stbuf->st_size = f->getSize();
		return 0;
	}

	DirDirIndexEntry * d = dynamic_cast<DirDirIndexEntry*>(entry.get());
	if (d)
	{
		// it's a dir!
		stbuf->st_mode = S_IFDIR | 0555; // r-xr-xr-x
		return 0;
	}

	// it's nothing?
	return -ENOENT;
}

int TerraFs::open(const string & path, struct fuse_file_info * fi)
{
	if ((fi->flags & 3) != O_RDONLY)
		return -EACCES;

	DirIndexEntry_ptr entry = getDirIndexEntry(path);
	if (entry == NULL)
		return -ENOENT;

	string * content = new string;

	if (curlie.getFile(baseUrl + path, *content) != 200)
	{
		delete content;
		return -ENOENT;
	}

	fi->fh = reinterpret_cast<uint64_t>(content);

	return 0;
}

int TerraFs::release(const string & path, struct fuse_file_info * fi)
{

	string * content = reinterpret_cast<string*>(fi->fh);
	delete content;
	return 0;
}

int TerraFs::read(const string & path, char * buf, size_t size, off_t offset,
		struct fuse_file_info * fi)
{
	if ((fi->flags & 3) != O_RDONLY)
		return -EACCES;

	DirIndexEntry_ptr entry = getDirIndexEntry(path);
	if (entry == NULL)
		return -ENOENT;

	string * content = reinterpret_cast<string*>(fi->fh);

	size_t len = content->size();
	if (offset < len)
	{
		if (offset + size > len)
			size = len - offset;
		memcpy(buf, content->data() + offset, size);
	}
	else
	{
		size = 0;
	}
	return size;
}

static int terrafs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		off_t offset, struct fuse_file_info *fi)
{
	return static_cast<TerraFs*>(fuse_get_context()->private_data)->readDir(
			string(path), buf, filler, offset, fi);
}

static int terrafs_getattr(const char *path, struct stat *stbuf)
{
	return static_cast<TerraFs*>(fuse_get_context()->private_data)->getAttr(
			string(path), stbuf);
}

static int terrafs_open(const char *path, struct fuse_file_info *fi)
{
	return static_cast<TerraFs*>(fuse_get_context()->private_data)->open(
			string(path), fi);
}

static int terrafs_read(const char *path, char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	return static_cast<TerraFs*>(fuse_get_context()->private_data)->read(
			string(path), buf, size, offset, fi);
}

static int terrafs_release(const char* path, struct fuse_file_info *fi)
{
	return static_cast<TerraFs*>(fuse_get_context()->private_data)->release(
			string(path), fi);
}

static struct fuse_operations oper =
{ };

struct terrafs_config
{
	char * server;
	bool staticRoot;
};
#define MYFS_OPT(t, p, v) { t, offsetof(struct terrafs_config, p), v }
static struct fuse_opt terrafs_opts[] =
{
  MYFS_OPT("server=%s", server, 0),
  MYFS_OPT("staticroot", staticRoot, 1),
  MYFS_OPT("nostaticroot", staticRoot, 0),
  MYFS_OPT("--staticroot=true", staticRoot, 1),
  MYFS_OPT("--staticroot=false", staticRoot, 0),
};

static int terrafs_opt_proc(void *data, const char *arg, int key,
		struct fuse_args *outargs)
{
	switch (key)
	{

	}

	return 1;
}

int main(int argc, char ** argv)
{
	oper.readdir = terrafs_readdir;
	oper.getattr = terrafs_getattr;
	oper.open = terrafs_open;
	oper.read = terrafs_read;
	oper.release = terrafs_release;

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct terrafs_config conf;
	memset(&conf, 0, sizeof(conf));

	fuse_opt_parse(&args, &conf, terrafs_opts, terrafs_opt_proc);

	TerraFs * app = new TerraFs(conf.server,conf.staticRoot);
	return fuse_main(args.argc, args.argv, &oper, app);
}
