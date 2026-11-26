/* Bulwark engine - the std::filesystem pieces PanoramaEngine calls, for
 * nxdk, whose libc++ (LLVM 10) ships the <filesystem> header without its
 * library. ISC licence, see LICENSE.
 *
 * Paths are libc++'s POSIX form ('/' separators, no root names). Path
 * decomposition is plain string work; status, file_size and directory
 * iteration go through the POSIX shims in compat/ (stat, opendir), so a
 * relative name is relative to D:, the XBE's folder. Errors are reported
 * through the error_code when there is one; the throwing forms (no
 * error_code) can't throw here and just return the failed result. */
#include <filesystem>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" { /* the compat/ headers are C */
#include <dirent.h>
#include <sys/stat.h>
}

_LIBCPP_BEGIN_NAMESPACE_FILESYSTEM

namespace
{
using sv = path::__string_view;

bool is_sep(char c) { return c == '/'; }

/* length of the leading separators: the root directory, when non-zero */
size_t root_len(sv s)
{
	size_t n = 0;
	while (n < s.size() && is_sep(s[n]))
		n++;
	return n;
}

/* the filenames, in order, and whether the path ends in a separator after one */
void split(sv s, std::vector<sv> &names, bool &trailing)
{
	size_t i = root_len(s);
	trailing = false;
	while (i < s.size()) {
		size_t e = i;
		while (e < s.size() && !is_sep(s[e]))
			e++;
		names.push_back(s.substr(i, e - i));
		i = e;
		while (i < s.size() && is_sep(s[i]))
			i++, trailing = i == s.size();
	}
}

void set_error(error_code *ec, errc e)
{
	if (ec)
		*ec = make_error_code(e);
}

void clear_error(error_code *ec)
{
	if (ec)
		ec->clear();
}
} // namespace

sv path::__root_name() const { return sv(); }

sv path::__root_directory() const
{
	sv s = __pn_;
	return root_len(s) ? s.substr(0, 1) : sv();
}

sv path::__root_path_raw() const { return __root_directory(); }

sv path::__relative_path() const
{
	sv s = __pn_;
	return s.substr(root_len(s));
}

sv path::__filename() const
{
	sv s = __pn_;
	if (__relative_path().empty() || is_sep(s.back()))
		return sv();
	size_t i = s.size();
	while (i > 0 && !is_sep(s[i - 1]))
		i--;
	return s.substr(i);
}

sv path::__parent_path() const
{
	sv s = __pn_;
	size_t root = root_len(s);
	if (root == s.size())
		return s; /* "" or the root alone */
	size_t e = s.size();
	if (!is_sep(s[e - 1])) /* drop the last filename */
		while (e > root && !is_sep(s[e - 1]))
			e--;
	/* then the separators before it, down to the root directory */
	while (e > root && is_sep(s[e - 1]))
		e--;
	if (e == 0 && root)
		e = 1;
	return s.substr(0, e);
}

sv path::__stem() const
{
	sv f = __filename();
	if (f == "." || f == "..")
		return f;
	size_t dot = f.rfind('.');
	return dot == sv::npos || dot == 0 ? f : f.substr(0, dot);
}

sv path::__extension() const
{
	sv f = __filename();
	if (f == "." || f == "..")
		return sv();
	size_t dot = f.rfind('.');
	return dot == sv::npos || dot == 0 ? sv() : f.substr(dot);
}

int path::__compare(sv other) const
{
	sv a = __pn_, b = other;
	bool ra = root_len(a) != 0, rb = root_len(b) != 0;
	if (ra != rb)
		return ra ? 1 : -1;
	std::vector<sv> na, nb;
	bool ta, tb;
	split(a, na, ta);
	split(b, nb, tb);
	/* a trailing separator is one more, empty, element */
	if (ta)
		na.push_back(sv());
	if (tb)
		nb.push_back(sv());
	for (size_t i = 0; i < na.size() && i < nb.size(); i++)
		if (int c = na[i].compare(nb[i]))
			return c < 0 ? -1 : 1;
	if (na.size() != nb.size())
		return na.size() < nb.size() ? -1 : 1;
	return 0;
}

path path::lexically_normal() const
{
	sv s = __pn_;
	if (s.empty())
		return path();
	bool root = root_len(s) != 0, trailing;
	std::vector<sv> names, out;
	split(s, names, trailing);
	bool ends_sep = false;
	for (size_t i = 0; i < names.size(); i++) {
		bool last = i + 1 == names.size();
		sv n = names[i];
		if (n == ".") {
			ends_sep = true;
		} else if (n == "..") {
			if (!out.empty() && out.back() != "..") {
				out.pop_back();
				ends_sep = true;
			} else if (root && out.empty()) {
				ends_sep = true; /* ".." at the root is the root */
			} else {
				out.push_back(n);
				ends_sep = false; /* no separator after a final ".." */
			}
		} else {
			out.push_back(n);
			ends_sep = last && trailing;
		}
	}
	string_type r = root ? "/" : "";
	for (size_t i = 0; i < out.size(); i++) {
		if (i)
			r += '/';
		r.append(out[i].data(), out[i].size());
	}
	if (ends_sep && !out.empty())
		r += '/';
	if (r.empty())
		r = ".";
	return path(std::move(r));
}

/* the iterator walks: the root directory, each filename, then an empty
 * element for a trailing separator. __entry_ points into the path's string
 * (one character, the separator, for the root and the trailing element);
 * the end iterator's is empty, at the string's end. */
path::iterator path::begin() const
{
	iterator it;
	it.__path_ptr_ = this;
	it.__state_ = iterator::_BeforeBegin;
	it.__entry_ = sv(__pn_.data(), 0);
	it.__increment();
	return it;
}

path::iterator path::end() const
{
	iterator it;
	it.__path_ptr_ = this;
	it.__state_ = iterator::_AtEnd;
	it.__entry_ = sv(__pn_.data() + __pn_.size(), 0);
	return it;
}

path::iterator &path::iterator::__increment()
{
	sv s = __path_ptr_->native();
	size_t pos;
	if (__state_ == _BeforeBegin) {
		if (root_len(s)) {
			__state_ = _InRootDir;
			__entry_ = s.substr(0, 1);
			__stashed_elem_ = path("/");
			return *this;
		}
		pos = 0;
	} else if (__state_ == _InTrailingSep || __state_ == _AtEnd) {
		pos = s.size();
	} else {
		pos = (size_t)(__entry_.data() - s.data()) + __entry_.size();
	}
	bool after_name = __state_ == _InFilenames;
	size_t seps = pos;
	while (pos < s.size() && is_sep(s[pos]))
		pos++;
	if (pos < s.size()) {
		size_t e = pos;
		while (e < s.size() && !is_sep(s[e]))
			e++;
		__state_ = _InFilenames;
		__entry_ = s.substr(pos, e - pos);
		__stashed_elem_ = path(string_type(__entry_));
	} else if (after_name && seps < s.size()) {
		__state_ = _InTrailingSep;
		__entry_ = s.substr(s.size() - 1, 1);
		__stashed_elem_ = path();
	} else {
		__state_ = _AtEnd;
		__entry_ = sv(s.data() + s.size(), 0);
		__stashed_elem_ = path();
	}
	return *this;
}

path::iterator &path::iterator::__decrement()
{
	/* rare: walk forward from the start to the element before this one */
	const path *p = __path_ptr_;
	iterator it = p->begin(), prev = it;
	while (!(it.__entry_.data() == __entry_.data() && it.__state_ == __state_) && it.__state_ != _AtEnd) {
		prev = it;
		it.__increment();
	}
	*this = prev;
	return *this;
}

file_status __status(const path &p, error_code *ec)
{
	struct stat st;
	if (::stat(p.c_str(), &st) != 0) {
		set_error(ec, errc::no_such_file_or_directory);
		return file_status(file_type::not_found);
	}
	clear_error(ec);
	if (S_ISDIR(st.st_mode))
		return file_status(file_type::directory, perms::all);
	if (S_ISREG(st.st_mode))
		return file_status(file_type::regular, perms::all);
	return file_status(file_type::unknown, perms::all);
}

file_status __symlink_status(const path &p, error_code *ec) { return __status(p, ec); }

uintmax_t __file_size(const path &p, error_code *ec)
{
	struct stat st;
	if (::stat(p.c_str(), &st) != 0) {
		set_error(ec, errc::no_such_file_or_directory);
		return uintmax_t(-1);
	}
	if (!S_ISREG(st.st_mode)) {
		set_error(ec, S_ISDIR(st.st_mode) ? errc::is_a_directory : errc::not_supported);
		return uintmax_t(-1);
	}
	clear_error(ec);
	return (uintmax_t)st.st_size;
}

error_code directory_entry::__do_refresh() noexcept
{
	__data_.__reset();
	error_code ec;
	file_status st = __status(__p_, &ec);
	if (ec)
		return ec;
	__data_.__type_ = st.type();
	__data_.__non_sym_perms_ = __data_.__sym_perms_ = st.permissions();
	if (st.type() == file_type::regular)
		__data_.__size_ = __file_size(__p_, nullptr);
	__data_.__cache_type_ = _RefreshNonSymlink;
	return error_code();
}

class __dir_stream
{
public:
	DIR *dir = nullptr;
	path root;
	directory_entry entry;

	~__dir_stream()
	{
		if (dir)
			closedir(dir);
	}

	/* the next entry other than "." and ".."; false at the end */
	bool advance()
	{
		while (struct dirent *d = readdir(dir)) {
			if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
				continue;
			file_type t = d->d_type == DT_DIR   ? file_type::directory
			              : d->d_type == DT_REG ? file_type::regular
			                                    : file_type::none;
			entry.__assign_iter_entry(root / d->d_name, directory_entry::__create_iter_result(t));
			return true;
		}
		return false;
	}
};

directory_iterator::directory_iterator(const path &p, error_code *ec, directory_options)
{
	DIR *d = opendir(p.c_str());
	if (!d) {
		set_error(ec, errc::no_such_file_or_directory);
		return;
	}
	clear_error(ec);
	__imp_ = std::make_shared<__dir_stream>();
	__imp_->dir = d;
	__imp_->root = p;
	if (!__imp_->advance())
		__imp_.reset();
}

directory_iterator &directory_iterator::__increment(error_code *ec)
{
	clear_error(ec);
	if (__imp_ && !__imp_->advance())
		__imp_.reset();
	return *this;
}

const directory_entry &directory_iterator::__dereference() const { return __imp_->entry; }

_LIBCPP_END_NAMESPACE_FILESYSTEM
