/*
   Copyright (c) 2015, Facebook, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */
#pragma once

#include <log.h>
#include <sstream>
#include <string>

#include "rpcclient.hpp"

namespace myrocks_rpc {

class Rdb_logger : public rocksdb::Logger {
 public:
  explicit Rdb_logger(const rocksdb::InfoLogLevel log_level =
                          rocksdb::InfoLogLevel::ERROR_LEVEL)
      : m_mysql_log_level(log_level) {}

  void Logv(const rocksdb::InfoLogLevel log_level, const char *format,
            va_list ap) override {
    DBUG_ASSERT(format != nullptr);

    enum loglevel mysql_log_level;

    if (m_logger) {
      m_logger->Logv(log_level, format, ap);
    }

    if (log_level < m_mysql_log_level) {
      return;
    }

    if (log_level >= rocksdb::InfoLogLevel::ERROR_LEVEL) {
      mysql_log_level = ERROR_LEVEL;
    } else if (log_level >= rocksdb::InfoLogLevel::WARN_LEVEL) {
      mysql_log_level = WARNING_LEVEL;
    } else {
      mysql_log_level = INFORMATION_LEVEL;
    }

    // log to MySQL
    std::string f("LibRocksDB:");
    f.append(format);
    error_log_print(mysql_log_level, f.c_str(), ap);
  }

  void Logv(const char *format, va_list ap) override {
    DBUG_ASSERT(format != nullptr);
    // If no level is specified, it is by default at information level
    Logv(rocksdb::InfoLogLevel::INFO_LEVEL, format, ap);
  }

  // ALTER
  // void SetRocksDBLogger(const std::shared_ptr<rocksdb::Logger> logger) {
  //   m_logger = logger;
  // }
  void SetRocksDBLogger(rocksdb::Logger *logger) { m_logger = logger; }

  void SetInfoLogLevel(const rocksdb::InfoLogLevel log_level) override {
    // The InfoLogLevel for the logger is used by rocksdb to filter
    // messages, so it needs to be the lower of the two loggers
    rocksdb::InfoLogLevel base_level = log_level;

    // ALTER
    // if (m_logger && m_logger->GetInfoLogLevel() < base_level) {
    //   base_level = m_logger->GetInfoLogLevel();
    // }
    rocksdb::InfoLogLevel logger_level =
        rocksdb_Logger__GetInfoLogLevel(m_logger);
    if (m_logger && logger_level < base_level) {
      base_level = logger_level;
    }

    // ALTER
    // rocksdb::Logger::SetInfoLogLevel(base_level);
    rocksdb_Logger__SetInfoLogLevel(m_logger, base_level);

    m_mysql_log_level = log_level;
  }

 private:
  // ALTER
  // std::shared_ptr<rocksdb::Logger> m_logger;
  rocksdb::Logger *m_logger;
  rocksdb::InfoLogLevel m_mysql_log_level;
};

}  // namespace myrocks_rpc
