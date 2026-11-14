/* -*- mode: c++ -*- */
#ifndef R_PROGRESS_H
#define R_PROGRESS_H

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

#include <R.h>
#include <Rinternals.h>
#include <R_ext/Print.h>

namespace RProgress {

class RProgress {
public:
  RProgress(std::string format = "[:bar] :percent",
            double total = 100,
            int width = Rf_GetOptionWidth() - 2,
            char complete_char = '=',
            char incomplete_char = '-',
            bool clear = true,
            double show_after = 0.2)
      : format(std::move(format)),
        total(total),
        width(width),
        cursor_char(1, complete_char),
        complete_char(1, complete_char),
        incomplete_char(1, incomplete_char),
        clear(clear),
        show_after(show_after) {}

  void set_total(double v) { total = v; }
  void set_extra(int v) { extra = v; }
  void set_cursor_char(const char* v) { cursor_char = v; }

  void tick(double len = 1) {
    if (first) start = time_now();
    current += len;
    toupdate = toupdate || (time_now() - start > show_after);
    if (current >= total) complete = true;
    if (first || toupdate || complete) render();
    if (complete && supported) {
      if (clear) {
        clear_line(use_stderr, width);
        print_str(use_stderr, "\r");
      } else {
        print_str(use_stderr, "\n");
      }
    }
    first = false;
  }

  void update(double ratio) { tick(ratio * total - current); }

private:
  bool first = true;
  bool supported = is_supported();
  bool use_stderr = !is_r_studio();
  std::string format;
  double total;
  double current = 0;
  int extra = 0;
  int width;
  std::string cursor_char, complete_char, incomplete_char, last_draw;
  bool clear;
  double show_after;
  double start = 0;
  bool toupdate = false;
  bool complete = false;

  void render() {
    if (!supported) return;
    std::string str = format;
    double ratio_now = std::clamp(current / total, 0.0, 1.0);
    double percent = std::round(ratio_now * 100.0);

    std::ostringstream buf;
    buf << std::setw(3) << percent << "%";
    replace_all(str, ":percent", buf.str());

    double elapsed_secs = time_now() - start;
    replace_all(str, ":elapsed", vague_dt(elapsed_secs));
    double eta_secs = (percent == 100.0) ? 0.0 : elapsed_secs * (total / current - 1.0);
    replace_all(str, ":eta", std::isinf(eta_secs) ? "?s" : vague_dt(eta_secs));

    buf.str("");
    buf.clear();
    buf << std::setw(3) << extra << "%";
    replace_all(str, ":extra", buf.str());

    std::string str_no_bar = str;
    replace_all(str_no_bar, ":bar", "");
    long bar_width = std::max<long>(0, width - static_cast<long>(str_no_bar.length()));
    long complete_len = static_cast<long>(std::round(bar_width * ratio_now));

    std::string bar;
    for (long i = 0; i < complete_len - 1; i++) bar += complete_char;
    if (complete_len > 0) bar += cursor_char;
    for (long i = complete_len; i < bar_width; i++) bar += incomplete_char;
    replace_all(str, ":bar", bar);

    if (last_draw != str) {
      if (last_draw.length() > str.length()) clear_line(use_stderr, width);
      print_str(use_stderr, "\r");
      print_str(use_stderr, str.c_str());
      last_draw = std::move(str);
    }
  }

  static void print_str(bool to_stderr, const char* s) {
    if (to_stderr) REprintf("%s", s);
    else Rprintf("%s", s);
  }

  static void clear_line(bool to_stderr, int w) {
    std::string spaces(static_cast<size_t>(std::max(0, w)) + 1, ' ');
    spaces[0] = '\r';
    print_str(to_stderr, spaces.c_str());
  }

  static double time_now() {
    return std::chrono::duration<double>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  static void replace_all(std::string& str, const std::string& from, const std::string& to) {
    for (size_t pos = 0; (pos = str.find(from, pos)) != std::string::npos; pos += to.length()) {
      str.replace(pos, from.length(), to);
    }
  }

  static std::string vague_dt(double s) {
    std::ostringstream buf;
    buf << std::setw(2);
    if (s < 50) buf << std::round(s) << "s";
    else if (s / 60 < 50) buf << std::round(s / 60) << "m";
    else if (s / 3600 < 18) buf << std::round(s / 3600) << "h";
    else if (s / 86400 < 30) buf << std::round(s / 86400) << "d";
    else if (s / 86400 < 335) buf << std::round(s / (86400 * 30)) << "M";
    else buf << std::round(s / (86400 * 365.25)) << "y";
    return buf.str();
  }

  static bool is_r_studio() {
    const char* v = std::getenv("RSTUDIO");
    return (v && std::strcmp(v, "1") == 0) || std::getenv("RS_SHARED_SECRET") != nullptr;
  }

  static bool is_supported() {
    SEXP opt = PROTECT(Rf_GetOption1(Rf_install("progress_enabled")));
    bool enabled = Rf_isNull(opt) || R_compute_identical(opt, Rf_ScalarLogical(1), 16);
    UNPROTECT(1);
#ifdef _WIN32
    bool tty = _isatty(1) > 0;
#else
    bool tty = isatty(1) > 0;
#endif
    return enabled && (tty || is_r_studio() || std::getenv("R_GUI_APP_VERSION") != nullptr);
  }
};

} // namespace RProgress

#endif

