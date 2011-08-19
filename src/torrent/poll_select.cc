// libTorrent - BitTorrent library
// Copyright (C) 2005-2007, Jari Sundell
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// In addition, as a special exception, the copyright holders give
// permission to link the code of portions of this program with the
// OpenSSL library under certain conditions as described in each
// individual source file, and distribute linked combinations
// including the two.
//
// You must obey the GNU General Public License in all respects for
// all of the code used other than OpenSSL.  If you modify file(s)
// with this exception, you may extend this exception to your version
// of the file(s), but you are not obligated to do so.  If you do not
// wish to do so, delete this exception statement from your version.
// If you delete this exception statement from all source files in the
// program, then also delete it here.
//
// Contact:  Jari Sundell <jaris@ifi.uio.no>
//
//           Skomakerveien 33
//           3185 Skoppum, NORWAY

#include "config.h"

#include <algorithm>

#include <unistd.h>
#include <sys/time.h>

#include "net/socket_set.h"
#include "rak/allocators.h"

#include "event.h"
#include "exceptions.h"
#include "poll_select.h"
#include "thread_base.h"

namespace torrent {

template <typename _Operation>
struct poll_check_t {
  poll_check_t(Poll* p, fd_set* s, _Operation op) : m_poll(p), m_set(s), m_op(op) {}

  void operator () (Event* s) {
    // This check is nessesary as other events may remove a socket
    // from the set.
    if (s == NULL)
      return;

    // This check is not nessesary, just for debugging.
    if (s->file_descriptor() < 0)
      throw internal_error("poll_check: s->fd < 0");

    if (FD_ISSET(s->file_descriptor(), m_set)) {
      m_op(s);

      // We waive the global lock after an event has been processed in
      // order to ensure that 's' doesn't get removed before the op is
      // called.
      if ((m_poll->flags() & Poll::flag_waive_global_lock) && ThreadBase::global_queue_size() != 0)
        ThreadBase::waive_global_lock();
    }
  }

  Poll*      m_poll;
  fd_set*    m_set;
  _Operation m_op;
};

template <typename _Operation>
inline poll_check_t<_Operation>
poll_check(Poll* p, fd_set* s, _Operation op) {
  return poll_check_t<_Operation>(p, s, op);
}

struct poll_mark {
  poll_mark(fd_set* s, unsigned int* m) : m_max(m), m_set(s) {}

  void operator () (Event* s) {
    // Neither of these checks are nessesary, just for debugging.
    if (s == NULL)
      throw internal_error("poll_mark: s == NULL");

    if (s->file_descriptor() < 0)
      throw internal_error("poll_mark: s->fd < 0");

    *m_max = std::max(*m_max, (unsigned int)s->file_descriptor());

    FD_SET(s->file_descriptor(), m_set);
  }

  unsigned int*       m_max;
  fd_set*             m_set;
};

PollSelect*
PollSelect::create(int maxOpenSockets) {
  if (maxOpenSockets <= 0)
    throw internal_error("PollSelect::set_open_max(...) received an invalid value");

  // Just a temp hack, make some special template function for this...
  //
  // Also consider how portable this is for specialized C++
  // allocators.
  struct block_type {
    PollSelect t1;
    SocketSet t2;
    SocketSet t3;
    SocketSet t4;
  };

  rak::cacheline_allocator<Block*> cl_alloc;
  block_type* block = new (cl_alloc) block_type;

  PollSelect* p = new (&block->t1) PollSelect;

  p->m_readSet = new (&block->t2) SocketSet;
  p->m_writeSet = new (&block->t3) SocketSet;
  p->m_exceptSet = new (&block->t4) SocketSet;

  p->m_readSet->reserve(maxOpenSockets);
  p->m_writeSet->reserve(maxOpenSockets);
  p->m_exceptSet->reserve(maxOpenSockets);

  return p;
}

PollSelect::~PollSelect() {
  m_readSet->prepare();
  m_writeSet->prepare();
  m_exceptSet->prepare();

  // Re-add this check when you've cleaned up the client shutdown procedure.
  if (!m_readSet->empty() || !m_writeSet->empty() || !m_exceptSet->empty()) {
    throw internal_error("PollSelect::~PollSelect() called but the sets are not empty");

    // for (SocketSet::const_iterator itr = m_readSet->begin(); itr != m_readSet->end(); itr++)
    //   std::cout << "R" << (*itr)->file_descriptor() << std::endl;

    // for (SocketSet::const_iterator itr = m_writeSet->begin(); itr != m_writeSet->end(); itr++)
    //   std::cout << "W" << (*itr)->file_descriptor() << std::endl;

    // for (SocketSet::const_iterator itr = m_exceptSet->begin(); itr != m_exceptSet->end(); itr++)
    //   std::cout << "E" << (*itr)->file_descriptor() << std::endl;
  }

//   delete m_readSet;
//   delete m_writeSet;
//   delete m_exceptSet;

  m_readSet = m_writeSet = m_exceptSet = NULL;
}

uint32_t
PollSelect::open_max() const {
  return m_readSet->max_size();
}

unsigned int
PollSelect::fdset(fd_set* readSet, fd_set* writeSet, fd_set* exceptSet) {
  unsigned int maxFd = 0;

  m_readSet->prepare();
  std::for_each(m_readSet->begin(), m_readSet->end(), poll_mark(readSet, &maxFd));

  m_writeSet->prepare();
  std::for_each(m_writeSet->begin(), m_writeSet->end(), poll_mark(writeSet, &maxFd));
  
  m_exceptSet->prepare();
  std::for_each(m_exceptSet->begin(), m_exceptSet->end(), poll_mark(exceptSet, &maxFd));

  return maxFd;
}

void
PollSelect::perform(fd_set* readSet, fd_set* writeSet, fd_set* exceptSet) {
  // Make sure we don't do read/write on fd's that are in except. This should
  // not be a problem as any except call should remove it from the m_*Set's.
  m_exceptSet->prepare();
  std::for_each(m_exceptSet->begin(), m_exceptSet->end(),
		poll_check(this, exceptSet, std::mem_fun(&Event::event_error)));

  m_readSet->prepare();
  std::for_each(m_readSet->begin(), m_readSet->end(),
		poll_check(this, readSet, std::mem_fun(&Event::event_read)));

  m_writeSet->prepare();
  std::for_each(m_writeSet->begin(), m_writeSet->end(),
		poll_check(this, writeSet, std::mem_fun(&Event::event_write)));
}

inline static void
log_poll_open(Event* event) {
#ifdef LT_LOG_POLL_OPEN
  static int log_fd = -1;
  char buffer[256];

  if (log_fd == -1) {
    snprintf(buffer, 256, LT_LOG_POLL_OPEN, getpid());
    
    if ((log_fd = open(buffer, O_WRONLY | O_CREAT | O_TRUNC)) == -1)
      throw internal_error("Could not open poll open log file.");
  }

  unsigned int buf_lenght = snprintf(buffer, 256, "open %i\n",
                                     event->fd());

#endif
}

void
PollSelect::open(Event* event) {
  if ((uint32_t)event->file_descriptor() >= m_readSet->max_size())
    throw internal_error("Tried to add a socket to PollSelect that is larger than PollSelect::get_open_max()");

  if (in_read(event) || in_write(event) || in_error(event))
    throw internal_error("PollSelect::open(...) called on an inserted event");
}

void
PollSelect::close(Event* event) {
  if ((uint32_t)event->file_descriptor() >= m_readSet->max_size())
    throw internal_error("PollSelect::close(...) called with an invalid file descriptor");

  if (in_read(event) || in_write(event) || in_error(event))
    throw internal_error("PollSelect::close(...) called on an inserted event");
}

void
PollSelect::closed(Event* event) {
  // event->get_fd() was closed, remove it from the sets.
  m_readSet->erase(event);
  m_writeSet->erase(event);
  m_exceptSet->erase(event);
}

bool
PollSelect::in_read(Event* event) {
  return m_readSet->find(event) != m_readSet->end();
}

bool
PollSelect::in_write(Event* event) {
  return m_writeSet->find(event) != m_writeSet->end();
}

bool
PollSelect::in_error(Event* event) {
  return m_exceptSet->find(event) != m_exceptSet->end();
}

void
PollSelect::insert_read(Event* event) {
  m_readSet->insert(event);
}

void
PollSelect::insert_write(Event* event) {
  m_writeSet->insert(event);
}

void
PollSelect::insert_error(Event* event) {
  m_exceptSet->insert(event);
}

void
PollSelect::remove_read(Event* event) {
  m_readSet->erase(event);
}

void
PollSelect::remove_write(Event* event) {
  m_writeSet->erase(event);
}

void
PollSelect::remove_error(Event* event) {
  m_exceptSet->erase(event);
}

}
