/* GStreamer
 * Copyright (C) 1999,2000 Erik Walthinsen <omega@cse.ogi.edu>
 *                    2004 Wim Taymans <wim@fluendo.com>
 *
 * gstsystemclock.c: Default clock, uses the system clock
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:gstsystemclock
 * @title: GstSystemClock
 * @short_description: Default clock that uses the current system time
 * @see_also: #GstClock
 *
 * The GStreamer core provides a GstSystemClock based on the system time.
 * Asynchronous callbacks are scheduled from an internal thread.
 *
 * Clock implementors are encouraged to subclass this systemclock as it
 * implements the async notification.
 *
 * Subclasses can however override all of the important methods for sync and
 * async notifications to implement their own callback methods or blocking
 * wait operations.
 */

#include "gst_private.h"
#include "gstinfo.h"
#include "gstsystemclock.h"
#include "gstenumtypes.h"
#include "gstpoll.h"
#include "gstutils.h"
#include "glib-compat-private.h"

#include <errno.h>

#ifdef G_OS_WIN32
#  define WIN32_LEAN_AND_MEAN   /* prevents from including too many things */
#  include <windows.h>          /* QueryPerformance* stuff */
#  undef WIN32_LEAN_AND_MEAN
#  ifndef EWOULDBLOCK
#  define EWOULDBLOCK EAGAIN    /* This is just to placate gcc */
#  endif
#endif /* G_OS_WIN32 */

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#if defined __APPLE__
static struct mach_timebase_info mach_timebase;
#endif

#if defined G_OS_WIN32
static LARGE_INTEGER performance_counter_frequency;
#endif

/* Small helper to make the atomics below cheaper.
 *
 * GLib always uses SEQ_CST atomic ops while here it's more than enough to use
 * ACQUIRE/RELEASE atomic ops. On x86 / x86-64 the ACQUIRE load is compiling
 * to a simple memory read.
 */
#if defined __APPLE__ || defined G_OS_WIN32
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>

typedef atomic_int gst_atomic_int;

static inline int
gst_atomic_int_get_acquire (gst_atomic_int * x)
{
  return atomic_load_explicit (x, memory_order_acquire);
}

static inline void
gst_atomic_int_set_release (gst_atomic_int * x, gint val)
{
  atomic_store_explicit (x, val, memory_order_release);
}
#elif defined G_OS_WIN32
/* MSVC's C11 atomic might require special cflags
 * https://devblogs.microsoft.com/cppblog/c11-atomics-in-visual-studio-2022-version-17-5-preview-2/
 *
 * Can remove this code once below GLib MR is merged
 * https://gitlab.gnome.org/GNOME/glib/-/merge_requests/3436
 */

typedef LONG gst_atomic_int;

static inline int
gst_atomic_int_get_acquire (gst_atomic_int * x)
{
  return InterlockedAndAcquire (x, 1);
}

static inline void
gst_atomic_int_set_release (gst_atomic_int * x, gint val)
{
  InterlockedOrRelease (x, 1);
}
#else /* defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__) */
typedef int gst_atomic_int;
#define gst_atomic_int_get_acquire(x) g_atomic_int_get(x)
#define gst_atomic_int_set_release(x, val) g_atomic_int_set(x, val)
#endif /* defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__) */
#endif /* defined __APPLE__ || defined G_OS_WIN32 */

/* priv_gst_clock_init:
 *
 * Initialize internal state of the clock. This is safe to call multiple
 * times.
 */
void
priv_gst_clock_init (void)
{
#if defined __APPLE__
  static gst_atomic_int inited = FALSE;

  if (!gst_atomic_int_get_acquire (&inited)) {
    mach_timebase_info (&mach_timebase);
    gst_atomic_int_set_release (&inited, TRUE);
  }
#endif

#if defined G_OS_WIN32
  static gst_atomic_int inited = FALSE;

  if (!gst_atomic_int_get_acquire (&inited)) {
    QueryPerformanceFrequency (&performance_counter_frequency);
    gst_atomic_int_set_release (&inited, TRUE);
  }
#endif
}

GstClockTime
priv_gst_get_monotonic_time (void)
{
#if defined __APPLE__
  guint64 mach_t = mach_absolute_time ();
  return gst_util_uint64_scale (mach_t, mach_timebase.numer,
      mach_timebase.denom);
#elif defined G_OS_WIN32
  LARGE_INTEGER now;
  QueryPerformanceCounter (&now);

  return gst_util_uint64_scale (now.QuadPart, GST_SECOND,
      performance_counter_frequency.QuadPart);
#elif defined (HAVE_POSIX_TIMERS) && defined(HAVE_MONOTONIC_CLOCK) &&\
    defined (HAVE_CLOCK_GETTIME)
  struct timespec now;

  clock_gettime (CLOCK_MONOTONIC, &now);
  return GST_TIMESPEC_TO_TIME (now);
#else
  return g_get_monotonic_time () * 1000;
#endif
}

GstClockTime
priv_gst_get_real_time (void)
{
#if defined (HAVE_POSIX_TIMERS) && defined (HAVE_CLOCK_GETTIME)
  struct timespec now;

  clock_gettime (CLOCK_REALTIME, &now);
  return GST_TIMESPEC_TO_TIME (now);
#else
  return g_get_real_time () * 1000;
#endif
}

/* Define this to get some extra debug about jitter from each clock_wait */
#undef WAIT_DEBUGGING

#define GST_SYSTEM_CLOCK_GET_LOCK(clock)        GST_OBJECT_GET_LOCK(clock)
#define GST_SYSTEM_CLOCK_LOCK(clock)            g_mutex_lock(GST_SYSTEM_CLOCK_GET_LOCK(clock))
#define GST_SYSTEM_CLOCK_UNLOCK(clock)          g_mutex_unlock(GST_SYSTEM_CLOCK_GET_LOCK(clock))
#define GST_SYSTEM_CLOCK_GET_COND(clock)        (&GST_SYSTEM_CLOCK_CAST(clock)->priv->entries_changed)
#define GST_SYSTEM_CLOCK_WAIT(clock)            g_cond_wait(GST_SYSTEM_CLOCK_GET_COND(clock),GST_SYSTEM_CLOCK_GET_LOCK(clock))
#define GST_SYSTEM_CLOCK_BROADCAST(clock)       g_cond_broadcast(GST_SYSTEM_CLOCK_GET_COND(clock))

#if defined(HAVE_FUTEX) || defined(HAVE_FUTEX_TIME64)
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>

#if !defined(__NR_futex) && !defined(__NR_futex_time64)
#error "Neither __NR_futex nor __NR_futex_time64 are defined but were found by meson"
#endif

#ifndef FUTEX_WAIT_BITSET_PRIVATE
#define FUTEX_WAIT_BITSET_PRIVATE FUTEX_WAIT_BITSET
#endif
#ifndef FUTEX_WAKE_PRIVATE
#define FUTEX_WAKE_PRIVATE FUTEX_WAKE
#endif

#define GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)          (&(entry)->lock)
#define GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry)          (&(entry)->cond_val)
#define GST_SYSTEM_CLOCK_ENTRY_LOCK(entry)              (g_mutex_lock(GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)))
#define GST_SYSTEM_CLOCK_ENTRY_UNLOCK(entry)            (g_mutex_unlock(GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)))
#define GST_SYSTEM_CLOCK_ENTRY_WAIT_UNTIL(entry,ns)     gst_futex_cond_wait_until(GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry),GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry),(ns))
#define GST_SYSTEM_CLOCK_ENTRY_BROADCAST(entry)         gst_futex_cond_broadcast(GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry))

#define CLOCK_MIN_WAIT_TIME 100 /* ns */

typedef struct _GstClockEntryFutex GstClockEntryImpl;
struct _GstClockEntryFutex
{
  GstClockEntry entry;
  GWeakRef clock;
  GDestroyNotify destroy_entry;

  gboolean initialized;

  GMutex lock;
  guint cond_val;
};

static void
clear_entry (GstClockEntryImpl * entry)
{
  g_mutex_clear (&entry->lock);
}

static void
init_entry (GstClockEntryImpl * entry)
{
  g_mutex_init (&entry->lock);

  entry->destroy_entry = (GDestroyNotify) clear_entry;
}

static void
gst_futex_cond_broadcast (guint * cond_val)
{
  g_atomic_int_inc (cond_val);

#if defined(HAVE_FUTEX_TIME64)
#if defined(__BIONIC__)
  if (__builtin_available (android 30, *)) {
#else
  {
#endif
    int res;
    res = syscall (__NR_futex_time64, cond_val, (gsize) FUTEX_WAKE_PRIVATE,
        (gsize) INT_MAX, NULL);

    /* If the syscall does not exist (`ENOSYS`), we retry again below with the
     * normal `futex` syscall. This can happen if newer kernel headers are
     * used than the kernel that is actually running.
     */
#if defined(HAVE_FUTEX)
    if (res >= 0 || errno != ENOSYS) {
#else
    {
#endif
      return;
    }
  }
#endif

#if defined(HAVE_FUTEX)
  syscall (__NR_futex, cond_val, (gsize) FUTEX_WAKE_PRIVATE, (gsize) INT_MAX,
      NULL);
#endif
}

static gboolean
gst_futex_cond_wait_until (guint * cond_val, GMutex * mutex, gint64 end_time)
{
  guint sampled;
  int res;
  gboolean success;

  if (end_time < 0)
    return FALSE;

  sampled = *cond_val;
  g_mutex_unlock (mutex);

  /* `struct timespec` as defined by the libc headers does not necessarily
   * have any relation to the one used by the kernel for the `futex` syscall.
   *
   * Specifically, the libc headers might use 64-bit `time_t` while the kernel
   * headers use 32-bit `__kernel_old_time_t` on certain systems.
   *
   * To get around this problem we
   *   a) check if `futex_time64` is available, which only exists on 32-bit
   *      platforms and always uses 64-bit `time_t`.
   *   b) otherwise (or if that returns `ENOSYS`), we call the normal `futex`
   *      syscall with the `struct timespec_t` used by the kernel, which uses
   *      `__kernel_long_t` for both its fields. We use that instead of
   *      `__kernel_old_time_t` because it is equivalent and available in the
   *      kernel headers for a longer time.
   *
   * Also some 32-bit systems do not define `__NR_futex` at all and only
   * define `__NR_futex_time64`.
   */

#if defined(HAVE_FUTEX_TIME64)
#if defined(__BIONIC__)
  if (__builtin_available (android 30, *)) {
#else
  {
#endif
    struct
    {
      gint64 tv_sec;
      gint64 tv_nsec;
    } end;

    end.tv_sec = end_time / 1000000000;
    end.tv_nsec = end_time % 1000000000;

    /* we use FUTEX_WAIT_BITSET_PRIVATE rather than FUTEX_WAIT_PRIVATE to be
     * able to use absolute time */
    res =
        syscall (__NR_futex_time64, cond_val, (gsize) FUTEX_WAIT_BITSET_PRIVATE,
        (gsize) sampled, &end, NULL, FUTEX_BITSET_MATCH_ANY);

    /* If the syscall does not exist (`ENOSYS`), we retry again below with the
     * normal `futex` syscall. This can happen if newer kernel headers are
     * used than the kernel that is actually running.
     */
#if defined(HAVE_FUTEX)
    if (res >= 0 || errno != ENOSYS) {
#else
    {
#endif
      success = (res < 0 && errno == ETIMEDOUT) ? FALSE : TRUE;
      g_mutex_lock (mutex);

      return success;
    }
  }
#endif

#if defined(HAVE_FUTEX)
  {
    struct
    {
      __kernel_long_t tv_sec;
      __kernel_long_t tv_nsec;
    } end;

    /* Make sure to only ever call this if the end time actually fits into the
     * target type */
    g_assert (sizeof (__kernel_long_t) >= 8
        || end_time / 1000000000 <= G_MAXINT32);

    end.tv_sec = end_time / 1000000000;
    end.tv_nsec = end_time % 1000000000;

    /* we use FUTEX_WAIT_BITSET_PRIVATE rather than FUTEX_WAIT_PRIVATE to be
     * able to use absolute time */
    res =
        syscall (__NR_futex, cond_val, (gsize) FUTEX_WAIT_BITSET_PRIVATE,
        (gsize) sampled, &end, NULL, FUTEX_BITSET_MATCH_ANY);
    success = (res < 0 && errno == ETIMEDOUT) ? FALSE : TRUE;
    g_mutex_lock (mutex);

    return success;
  }
#endif

  /* We can't end up here because of the checks above */
  g_assert_not_reached ();
}

#elif defined (G_OS_UNIX)
#define GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)          (&(entry)->lock)
#define GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry)          (&(entry)->cond)
#define GST_SYSTEM_CLOCK_ENTRY_LOCK(entry)              (pthread_mutex_lock(GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)))
#define GST_SYSTEM_CLOCK_ENTRY_UNLOCK(entry)            (pthread_mutex_unlock(GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)))
#define GST_SYSTEM_CLOCK_ENTRY_WAIT_UNTIL(entry,ns)     gst_pthread_cond_wait_until(GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry),GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry),(ns))
#define GST_SYSTEM_CLOCK_ENTRY_BROADCAST(entry)         pthread_cond_broadcast(GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry))

#define CLOCK_MIN_WAIT_TIME 500 /* ns */

typedef struct _GstClockEntryPThread GstClockEntryImpl;
struct _GstClockEntryPThread
{
  GstClockEntry entry;
  GWeakRef clock;
  GDestroyNotify destroy_entry;

  gboolean initialized;

  pthread_cond_t cond;
  pthread_mutex_t lock;
};

static gboolean
gst_pthread_cond_wait_until (pthread_cond_t * cond, pthread_mutex_t * lock,
    guint64 end_time)
{
  struct timespec ts;
  gint status;

#if defined (HAVE_PTHREAD_CONDATTR_SETCLOCK) && defined (CLOCK_MONOTONIC)
  /* This is the exact check we used during init to set the clock to
   * monotonic, so if we're in this branch, timedwait() will already be
   * expecting a monotonic clock.
   */
  {
    ts.tv_sec = end_time / 1000000000;
    ts.tv_nsec = end_time % 1000000000;

    if ((status = pthread_cond_timedwait (cond, lock, &ts)) == 0)
      return TRUE;
  }
#elif defined (HAVE_PTHREAD_COND_TIMEDWAIT_RELATIVE_NP)
  /* end_time is given relative to the monotonic clock as returned by
   * g_get_monotonic_time().
   *
   * Since this pthreads wants the relative time, convert it back again.
   */
  {
    gint64 now = priv_gst_get_monotonic_time ();
    gint64 relative;

    if (end_time <= now)
      return FALSE;

    relative = end_time - now;

    ts.tv_sec = relative / 1000000000;
    ts.tv_nsec = relative % 1000000000;

    if ((status = pthread_cond_timedwait_relative_np (cond, lock, &ts)) == 0)
      return TRUE;
  }
#else
#error Cannot use pthread condition variables on your platform.
#endif

  if (G_UNLIKELY (status != ETIMEDOUT)) {
    g_error ("pthread_cond_timedwait returned %d", status);
  }

  return FALSE;
}

static void
clear_entry (GstClockEntryImpl * entry)
{
  pthread_cond_destroy (&entry->cond);
  pthread_mutex_destroy (&entry->lock);
}

static void
init_entry (GstClockEntryImpl * entry)
{
  pthread_mutexattr_t *m_pattr = NULL;
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
  pthread_mutexattr_t m_attr;
#endif
  pthread_condattr_t c_attr;
  gint status;

  pthread_condattr_init (&c_attr);

#if defined (HAVE_PTHREAD_CONDATTR_SETCLOCK) && defined (CLOCK_MONOTONIC)
  status = pthread_condattr_setclock (&c_attr, CLOCK_MONOTONIC);
  if (G_UNLIKELY (status != 0)) {
    g_error ("pthread_condattr_setclock returned %d", status);
  }
#elif defined (HAVE_PTHREAD_COND_TIMEDWAIT_RELATIVE_NP)
#else
#error Cannot use pthread condition variables on your platform.
#endif

  status = pthread_cond_init (&entry->cond, &c_attr);
  if (G_UNLIKELY (status != 0)) {
    g_error ("pthread_cond_init returned %d", status);
  }

  pthread_condattr_destroy (&c_attr);

#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
  pthread_mutexattr_init (&m_attr);
  pthread_mutexattr_settype (&m_attr, PTHREAD_MUTEX_ADAPTIVE_NP);
  m_pattr = &m_attr;
#endif

  status = pthread_mutex_init (&entry->lock, m_pattr);
  if (G_UNLIKELY (status != 0)) {
    g_error ("pthread_mutex_init returned %d", status);
  }
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
  pthread_mutexattr_destroy (&m_attr);
#endif

  entry->destroy_entry = (GDestroyNotify) clear_entry;
}
#else
#define GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)          (&(entry)->lock)
#define GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry)          (&(entry)->cond)
#define GST_SYSTEM_CLOCK_ENTRY_LOCK(entry)              (g_mutex_lock(GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)))
#define GST_SYSTEM_CLOCK_ENTRY_UNLOCK(entry)            (g_mutex_unlock(GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry)))
#define GST_SYSTEM_CLOCK_ENTRY_WAIT_UNTIL(entry,ns)     g_cond_wait_until(GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry),GST_SYSTEM_CLOCK_ENTRY_GET_LOCK(entry),((ns) / 1000))
#define GST_SYSTEM_CLOCK_ENTRY_BROADCAST(entry)         g_cond_broadcast(GST_SYSTEM_CLOCK_ENTRY_GET_COND(entry))

#if defined (G_OS_WIN32)
/* min wait time is 1ms on windows with GCond */
#define CLOCK_MIN_WAIT_TIME GST_MSECOND
#else
/* min wait time is 1us on non-windows with GCond */
#define CLOCK_MIN_WAIT_TIME GST_USECOND
#endif

typedef struct _GstClockEntryGLib GstClockEntryImpl;
struct _GstClockEntryGLib
{
  GstClockEntry entry;
  GWeakRef clock;
  GDestroyNotify destroy_entry;

  gboolean initialized;

  GMutex lock;
  GCond cond;
};

static void
clear_entry (GstClockEntryImpl * entry)
{
  g_cond_clear (&entry->cond);
  g_mutex_clear (&entry->lock);
}

static void
init_entry (GstClockEntryImpl * entry)
{
  g_cond_init (&entry->cond);
  g_mutex_init (&entry->lock);

  entry->destroy_entry = (GDestroyNotify) clear_entry;
}
#endif

/* check that our impl is smaller than what will be allocated by gstclock.c */
G_STATIC_ASSERT (sizeof (GstClockEntryImpl) <=
    sizeof (struct _GstClockEntryImpl));

/* Must be called with clock lock */
static inline void
ensure_entry_initialized (GstClockEntryImpl * entry_impl)
{
  if (!entry_impl->initialized) {
    init_entry (entry_impl);
    entry_impl->initialized = TRUE;
  }
}

struct _GstSystemClockPrivate
{
  GThread *thread;              /* thread for async notify */
  gboolean starting;
  gboolean stopping;

  GList *entries;
  GCond entries_changed;

  GstClockType clock_type;
};

#ifdef HAVE_POSIX_TIMERS
# ifdef HAVE_MONOTONIC_CLOCK
#  define DEFAULT_CLOCK_TYPE GST_CLOCK_TYPE_MONOTONIC
# else
#  define DEFAULT_CLOCK_TYPE GST_CLOCK_TYPE_REALTIME
# endif
#else
#define DEFAULT_CLOCK_TYPE GST_CLOCK_TYPE_MONOTONIC
#endif

enum
{
  PROP_0,
  PROP_CLOCK_TYPE,
  /* FILL ME */
};

/* the one instance of the systemclock */
static GstClock *_the_system_clock = NULL;
static gboolean _external_default_clock = FALSE;

static void gst_system_clock_dispose (GObject * object);
static void gst_system_clock_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_system_clock_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstClockTime gst_system_clock_get_internal_time (GstClock * clock);
static guint64 gst_system_clock_get_resolution (GstClock * clock);
static GstClockReturn gst_system_clock_id_wait_jitter (GstClock * clock,
    GstClockEntry * entry, GstClockTimeDiff * jitter);
static GstClockReturn gst_system_clock_id_wait_jitter_unlocked
    (GstClock * clock, GstClockEntry * entry, GstClockTimeDiff * jitter,
    gboolean restart);
static GstClockReturn gst_system_clock_id_wait_async (GstClock * clock,
    GstClockEntry * entry);
static void gst_system_clock_id_unschedule (GstClock * clock,
    GstClockEntry * entry);
static void gst_system_clock_async_thread (GstClock * clock);
static gboolean gst_system_clock_start_async (GstSystemClock * clock);

static GMutex _gst_sysclock_mutex;

/* static guint gst_system_clock_signals[LAST_SIGNAL] = { 0 }; */

#define gst_system_clock_parent_class parent_class
G_DEFINE_TYPE_WITH_PRIVATE (GstSystemClock, gst_system_clock, GST_TYPE_CLOCK);

static void
gst_system_clock_class_init (GstSystemClockClass * klass)
{
  GObjectClass *gobject_class;
  GstClockClass *gstclock_class;

  gobject_class = (GObjectClass *) klass;
  gstclock_class = (GstClockClass *) klass;

  gobject_class->dispose = gst_system_clock_dispose;
  gobject_class->set_property = gst_system_clock_set_property;
  gobject_class->get_property = gst_system_clock_get_property;

  g_object_class_install_property (gobject_class, PROP_CLOCK_TYPE,
      g_param_spec_enum ("clock-type", "Clock type",
          "The type of underlying clock implementation used",
          GST_TYPE_CLOCK_TYPE, DEFAULT_CLOCK_TYPE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstclock_class->get_internal_time = gst_system_clock_get_internal_time;
  gstclock_class->get_resolution = gst_system_clock_get_resolution;
  gstclock_class->wait = gst_system_clock_id_wait_jitter;
  gstclock_class->wait_async = gst_system_clock_id_wait_async;
  gstclock_class->unschedule = gst_system_clock_id_unschedule;
}

static void
gst_system_clock_init (GstSystemClock * clock)
{
  GstSystemClockPrivate *priv;

  GST_OBJECT_FLAG_SET (clock,
      GST_CLOCK_FLAG_CAN_DO_SINGLE_SYNC |
      GST_CLOCK_FLAG_CAN_DO_SINGLE_ASYNC |
      GST_CLOCK_FLAG_CAN_DO_PERIODIC_SYNC |
      GST_CLOCK_FLAG_CAN_DO_PERIODIC_ASYNC);

  clock->priv = priv = gst_system_clock_get_instance_private (clock);

  priv->clock_type = DEFAULT_CLOCK_TYPE;

  priv->entries = NULL;
  g_cond_init (&priv->entries_changed);

#if 0
  /* Uncomment this to start the async clock thread straight away */
  GST_SYSTEM_CLOCK_LOCK (clock);
  gst_system_clock_start_async (clock);
  GST_SYSTEM_CLOCK_UNLOCK (clock);
#endif
}

static void
gst_system_clock_dispose (GObject * object)
{
  GstClock *clock = (GstClock *) object;
  GstSystemClock *sysclock = GST_SYSTEM_CLOCK_CAST (clock);
  GstSystemClockPrivate *priv = sysclock->priv;
  GList *entries;

  /* else we have to stop the thread */
  GST_SYSTEM_CLOCK_LOCK (clock);
  priv->stopping = TRUE;
  /* unschedule all entries */
  for (entries = priv->entries; entries; entries = g_list_next (entries)) {
    GstClockEntryImpl *entry = (GstClockEntryImpl *) entries->data;

    /* We don't need to take the entry lock here because the async thread
     * would only ever look at the head entry, which is locked below and only
     * accesses new entries with the clock lock, which we hold here.
     */
    GST_CLOCK_ENTRY_STATUS ((GstClockEntry *) entry) = GST_CLOCK_UNSCHEDULED;

    /* Wake up only the head entry: the async thread would only be waiting for
     * this one, not all of them. Once the head entry is unscheduled it tries
     * to get the system clock lock (which we hold here) and then look for the
     * next entry. Once it gets the lock it will notice that all further
     * entries are unscheduled, would remove them one by one from the list and
     * then shut down. */
    if (!entries->prev) {
      /* it was initialized before adding to the list */
      g_assert (entry->initialized);

      GST_SYSTEM_CLOCK_ENTRY_LOCK (entry);
      GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "unscheduling entry %p",
          entry);
      GST_SYSTEM_CLOCK_ENTRY_BROADCAST (entry);
      GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) entry);
    }
  }
  GST_SYSTEM_CLOCK_BROADCAST (clock);
  GST_SYSTEM_CLOCK_UNLOCK (clock);

  if (priv->thread)
    g_thread_join (priv->thread);
  priv->thread = NULL;
  GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "joined thread");

  g_list_foreach (priv->entries, (GFunc) gst_clock_id_unref, NULL);
  g_list_free (priv->entries);
  priv->entries = NULL;

  g_cond_clear (&priv->entries_changed);

  G_OBJECT_CLASS (parent_class)->dispose (object);

  if (_the_system_clock == clock) {
    _the_system_clock = NULL;
    GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "disposed system clock");
  }
}

static void
gst_system_clock_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSystemClock *sysclock = GST_SYSTEM_CLOCK (object);

  switch (prop_id) {
    case PROP_CLOCK_TYPE:
      sysclock->priv->clock_type = (GstClockType) g_value_get_enum (value);
      GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, sysclock, "clock-type set to %d",
          sysclock->priv->clock_type);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_system_clock_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstSystemClock *sysclock = GST_SYSTEM_CLOCK (object);

  switch (prop_id) {
    case PROP_CLOCK_TYPE:
      g_value_set_enum (value, sysclock->priv->clock_type);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/**
 * gst_system_clock_set_default:
 * @new_clock: (allow-none): a #GstClock
 *
 * Sets the default system clock that can be obtained with
 * gst_system_clock_obtain().
 *
 * This is mostly used for testing and debugging purposes when you
 * want to have control over the time reported by the default system
 * clock.
 *
 * MT safe.
 *
 * Since: 1.4
 */
void
gst_system_clock_set_default (GstClock * new_clock)
{
  GstClock *clock;

  g_mutex_lock (&_gst_sysclock_mutex);
  clock = _the_system_clock;

  if (clock != NULL)
    gst_object_unref (clock);

  if (new_clock == NULL) {
    GST_CAT_DEBUG (GST_CAT_CLOCK, "resetting default system clock");
    _external_default_clock = FALSE;
  } else {
    GST_CAT_DEBUG (GST_CAT_CLOCK, "setting new default system clock to %p",
        new_clock);
    _external_default_clock = TRUE;
    g_object_ref (new_clock);
  }
  _the_system_clock = new_clock;
  g_mutex_unlock (&_gst_sysclock_mutex);
}

/**
 * gst_system_clock_obtain:
 *
 * Get a handle to the default system clock. The refcount of the
 * clock will be increased so you need to unref the clock after
 * usage.
 *
 * Returns: (transfer full): the default clock.
 *
 * MT safe.
 */
GstClock *
gst_system_clock_obtain (void)
{
  GstClock *clock;

  g_mutex_lock (&_gst_sysclock_mutex);
  clock = _the_system_clock;

  if (clock == NULL) {
    GST_CAT_DEBUG (GST_CAT_CLOCK, "creating new static system clock");
    g_assert (!_external_default_clock);
    clock = g_object_new (GST_TYPE_SYSTEM_CLOCK,
        "name", "GstSystemClock", NULL);

    /* Clear floating flag */
    gst_object_ref_sink (clock);
    GST_OBJECT_FLAG_SET (clock, GST_OBJECT_FLAG_MAY_BE_LEAKED);
    _the_system_clock = clock;
    g_mutex_unlock (&_gst_sysclock_mutex);
  } else {
    g_mutex_unlock (&_gst_sysclock_mutex);
    GST_CAT_DEBUG (GST_CAT_CLOCK, "returning static system clock");
  }

  /* we ref it since we are a clock factory. */
  gst_object_ref (clock);
  return clock;
}

/* this thread reads the sorted clock entries from the queue.
 *
 * It waits on each of them and fires the callback when the timeout occurs.
 *
 * When an entry in the queue was canceled before we wait for it, it is
 * simply skipped.
 *
 * When waiting for an entry, it can become canceled, in that case we don't
 * call the callback but move to the next item in the queue.
 *
 * MT safe.
 */
static void
gst_system_clock_async_thread (GstClock * clock)
{
  GstSystemClock *sysclock = GST_SYSTEM_CLOCK_CAST (clock);
  GstSystemClockPrivate *priv = sysclock->priv;
  GstClockReturn status;

  GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "enter system clock thread");
  GST_SYSTEM_CLOCK_LOCK (clock);
  /* signal spinup */
  priv->starting = FALSE;
  GST_SYSTEM_CLOCK_BROADCAST (clock);
  /* now enter our (almost) infinite loop */
  while (!priv->stopping) {
    GstClockEntry *entry;
    GstClockTime requested;
    GstClockReturn res;

    /* check if something to be done */
    while (priv->entries == NULL) {
      GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
          "no clock entries, waiting..");
      /* wait for work to do */
      GST_SYSTEM_CLOCK_WAIT (clock);
      GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "got signal");
      /* clock was stopping, exit */
      if (priv->stopping)
        goto exit;
    }

    /* pick the next entry */
    entry = priv->entries->data;

    /* it was initialized before adding to the list */
    g_assert (((GstClockEntryImpl *) entry)->initialized);

    /* unlocked before the next loop iteration at latest */
    GST_SYSTEM_CLOCK_ENTRY_LOCK ((GstClockEntryImpl *) entry);

    /* set entry status to busy before we release the clock lock */
    status = GST_CLOCK_ENTRY_STATUS (entry);

    /* check for unscheduled */
    if (G_UNLIKELY (status == GST_CLOCK_UNSCHEDULED)) {
      /* entry was unscheduled, move to the next one */
      GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
          "async entry %p unscheduled", entry);
      GST_SYSTEM_CLOCK_UNLOCK (clock);
      goto unlock_entry_and_next_entry;
    }

    /* for periodic timers, status can be EARLY from a previous run */
    if (G_UNLIKELY (status != GST_CLOCK_OK && status != GST_CLOCK_EARLY))
      GST_CAT_ERROR_OBJECT (GST_CAT_CLOCK, clock,
          "unexpected status %d for entry %p", status, entry);

    /* mark the entry as busy */
    GST_CLOCK_ENTRY_STATUS (entry) = GST_CLOCK_BUSY;

    requested = entry->time;

    /* needs to be locked again before the next loop iteration, and we only
     * unlock it here so that gst_system_clock_id_wait_async() is guaranteed
     * to see status==BUSY later and wakes up this thread, and dispose() does
     * not override BUSY with UNSCHEDULED here. */
    GST_SYSTEM_CLOCK_UNLOCK (clock);

    GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "waiting on entry %p", entry);

    /* now wait for the entry */
    res =
        gst_system_clock_id_wait_jitter_unlocked (clock, (GstClockID) entry,
        NULL, FALSE);

    switch (res) {
      case GST_CLOCK_UNSCHEDULED:
        /* entry was unscheduled, move to the next */
        GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
            "async entry %p unscheduled", entry);
        goto unlock_entry_and_next_entry;
      case GST_CLOCK_OK:
      case GST_CLOCK_EARLY:
      {
        GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) entry);
        /* entry timed out normally, fire the callback and move to the next
         * entry */
        GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "async entry %p timed out",
            entry);
        if (entry->func) {
          /* unlock before firing the callback */
          entry->func (clock, entry->time, (GstClockID) entry,
              entry->user_data);
        }
        if (entry->type == GST_CLOCK_ENTRY_PERIODIC) {
          GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
              "updating periodic entry %p", entry);

          GST_SYSTEM_CLOCK_LOCK (clock);
          /* adjust time now */
          entry->time = requested + entry->interval;
          /* and resort the list now */
          priv->entries =
              g_list_sort (priv->entries, gst_clock_id_compare_func);
          /* and restart */
          continue;
        } else {
          GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "moving to next entry");
          goto next_entry;
        }
      }
      case GST_CLOCK_BUSY:
        /* somebody unlocked the entry but is was not canceled, This means that
         * a new entry was added in front of the queue. Pick the new head
         * entry of the list and continue waiting. */
        GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
            "async entry %p needs restart", entry);

        /* we set the entry back to the OK state. This is needed so that the
         * _unschedule() code can see if an entry is currently being waited
         * on (when its state is BUSY). */
        GST_CLOCK_ENTRY_STATUS (entry) = GST_CLOCK_OK;
        GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) entry);
        GST_SYSTEM_CLOCK_LOCK (clock);
        continue;
      default:
        GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
            "strange result %d waiting for %p, skipping", res, entry);
        g_warning ("%s: strange result %d waiting for %p, skipping",
            GST_OBJECT_NAME (clock), res, entry);
        goto unlock_entry_and_next_entry;
    }
  unlock_entry_and_next_entry:
    GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) entry);
  next_entry:
    GST_SYSTEM_CLOCK_LOCK (clock);

    /* we remove the current entry and unref it */
    priv->entries = g_list_remove (priv->entries, entry);
    gst_clock_id_unref ((GstClockID) entry);
  }
exit:
  /* signal exit */
  GST_SYSTEM_CLOCK_BROADCAST (clock);
  GST_SYSTEM_CLOCK_UNLOCK (clock);
  GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "exit system clock thread");
}

#ifdef HAVE_POSIX_TIMERS
static inline clockid_t
clock_type_to_posix_id (GstClockType clock_type)
{
#ifdef HAVE_MONOTONIC_CLOCK
  if (clock_type == GST_CLOCK_TYPE_MONOTONIC)
    return CLOCK_MONOTONIC;
#endif
  if (clock_type == GST_CLOCK_TYPE_TAI)
#ifdef CLOCK_TAI
    return CLOCK_TAI;
#else
    GST_ERROR
        ("No CLOCK_TAI available on the system. Falling back to CLOCK_REALTIME");
#endif
  return CLOCK_REALTIME;
}
#endif

/* MT safe */
static GstClockTime
gst_system_clock_get_internal_time (GstClock * clock)
{
  GstSystemClock *sysclock = GST_SYSTEM_CLOCK_CAST (clock);

  /* For the monotonic and realtime clock, always directly call the specific
   * helper functions above */
  if (sysclock->priv->clock_type == GST_CLOCK_TYPE_MONOTONIC)
    return priv_gst_get_monotonic_time ();
  else if (sysclock->priv->clock_type == GST_CLOCK_TYPE_REALTIME)
    return priv_gst_get_real_time ();

  /* If POSIX timers are available, use those for any other clock,
   * or otherwise return the monotonic time */
#if defined HAVE_POSIX_TIMERS && defined HAVE_CLOCK_GETTIME
  // BSD and Linux' Posix timers and clock_gettime cover all of the different clock types
  // without need for special handling so we'll use those.
  clockid_t ptype;
  struct timespec ts;

  ptype = clock_type_to_posix_id (sysclock->priv->clock_type);

  if (G_UNLIKELY (clock_gettime (ptype, &ts)))
    return GST_CLOCK_TIME_NONE;

  return GST_TIMESPEC_TO_TIME (ts);
#else /* !HAVE_POSIX_TIMERS || !HAVE_CLOCK_GETTIME */
  return priv_gst_get_monotonic_time ();
#endif /* !HAVE_POSIX_TIMERS || !HAVE_CLOCK_GETTIME */
}

static guint64
gst_system_clock_get_resolution (GstClock * clock)
{
  GstSystemClock *sysclock = GST_SYSTEM_CLOCK_CAST (clock);

#if defined G_OS_WIN32
  if (sysclock->priv->clock_type == GST_CLOCK_TYPE_REALTIME) {
    return 1 * GST_USECOND;
  } else {
    return GST_SECOND / performance_counter_frequency.QuadPart;
  }
#endif

#if defined __APPLE__
  // On Apple platforms we want to use mach_absolute_time() unconditionally
  // for the monotonic clock even if clock_gettime() support is available.
  // Only use the latter for other clock types there.
  if (sysclock->priv->clock_type == GST_CLOCK_TYPE_MONOTONIC) {
    return gst_util_uint64_scale (GST_NSECOND,
        mach_timebase.numer, mach_timebase.denom);
  }
#endif

#if defined(HAVE_POSIX_TIMERS) && defined(HAVE_CLOCK_GETTIME)
  {
    clockid_t ptype;
    struct timespec ts;

    ptype = clock_type_to_posix_id (sysclock->priv->clock_type);

    if (G_UNLIKELY (clock_getres (ptype, &ts)))
      return GST_CLOCK_TIME_NONE;

    return GST_TIMESPEC_TO_TIME (ts);
  }
#endif /* HAVE_POSIX_TIMERS && HAVE_CLOCK_GETTIME */

  return 1 * GST_USECOND;
}

/* synchronously wait on the given GstClockEntry.
 *
 * We do this by blocking on the entry specifically rather than a global
 * condition variable so that each possible thread may be woken up
 * individually. This ensures that we don't wake up possibly multiple threads
 * when unscheduling an entry.
 *
 * Entries that arrive too late are simply not waited on and a
 * GST_CLOCK_EARLY result is returned.
 *
 * This is called with the ENTRY_LOCK but not SYSTEM_CLOCK_LOCK!
 *
 * MT safe.
 */
static GstClockReturn
gst_system_clock_id_wait_jitter_unlocked (GstClock * clock,
    GstClockEntry * entry, GstClockTimeDiff * jitter, gboolean restart)
{
  GstClockTime entryt, now;
  GstClockTimeDiff diff;
  GstClockReturn status;
  gint64 mono_ts;

  /* Getting the time from the clock locks the clock, so without unlocking the
   * entry we would have a lock order violation here that can lead to deadlocks.
   *
   * It's not a problem to take the mutex again after getting the times (which
   * might block for a moment) as waiting happens based on the absolute time.
   */
  GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) entry);

  /* need to call the overridden method because we want to sync against the time
   * of the clock, whatever the subclass uses as a clock. */
  now = gst_clock_get_time (clock);
  mono_ts = g_get_monotonic_time ();

  GST_SYSTEM_CLOCK_ENTRY_LOCK ((GstClockEntryImpl *) entry);
  /* Might have been unscheduled in the meantime */
  status = GST_CLOCK_ENTRY_STATUS (entry);
  if (G_UNLIKELY (status == GST_CLOCK_UNSCHEDULED)) {
    return GST_CLOCK_UNSCHEDULED;
  }

  /* get the time of the entry */
  entryt = GST_CLOCK_ENTRY_TIME (entry);

  /* the diff of the entry with the clock is the amount of time we have to
   * wait */
  diff = GST_CLOCK_DIFF (now, entryt);
  if (G_LIKELY (jitter))
    *jitter = -diff;

  GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "entry %p"
      " time %" GST_TIME_FORMAT
      " now %" GST_TIME_FORMAT
      " diff (time-now) %" G_GINT64_FORMAT,
      entry, GST_TIME_ARGS (entryt), GST_TIME_ARGS (now), diff);

  if (G_LIKELY (diff > CLOCK_MIN_WAIT_TIME)) {
#ifdef WAIT_DEBUGGING
    GstClockTime final;
#endif

    while (TRUE) {
      gboolean waitret;

#ifdef HAVE_CLOCK_NANOSLEEP
      if (diff <= 500 * GST_USECOND) {
        /* In order to provide more accurate wait, we will use BLOCKING
           clock_nanosleep for any deadlines at or below 500us */
        struct timespec end;
        GST_TIME_TO_TIMESPEC (mono_ts * 1000 + diff, end);
        GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) entry);
        waitret =
            clock_nanosleep (CLOCK_MONOTONIC, TIMER_ABSTIME, &end, NULL) == 0;
        GST_SYSTEM_CLOCK_ENTRY_LOCK ((GstClockEntryImpl *) entry);
      } else {

        if (diff < 2 * GST_MSECOND) {
          /* For any deadline within 2ms, we first use the regular non-blocking
             wait by reducing the diff accordingly */
          diff -= 500 * GST_USECOND;
        }
#endif

        /* now wait on the entry, it either times out or the cond is signalled.
         * The status of the entry is BUSY only around the wait. */
        waitret =
            GST_SYSTEM_CLOCK_ENTRY_WAIT_UNTIL ((GstClockEntryImpl *) entry,
            mono_ts * 1000 + diff);

#ifdef HAVE_CLOCK_NANOSLEEP
      }
#endif

      /* get the new status, mark as DONE. We do this so that the unschedule
       * function knows when we left the poll and doesn't need to wakeup the
       * poll anymore. */
      status = GST_CLOCK_ENTRY_STATUS (entry);
      /* we were unscheduled, exit immediately */
      if (G_UNLIKELY (status == GST_CLOCK_UNSCHEDULED))
        break;
      if (G_UNLIKELY (status != GST_CLOCK_BUSY))
        GST_CAT_ERROR_OBJECT (GST_CAT_CLOCK, clock,
            "unexpected status %d for entry %p", status, entry);
      GST_CLOCK_ENTRY_STATUS (entry) = GST_CLOCK_DONE;

      GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
          "entry %p unlocked, status %d", entry, status);

      if (G_UNLIKELY (status == GST_CLOCK_UNSCHEDULED)) {
        goto done;
      } else {
        if (waitret) {
          /* some other id got unlocked */
          if (!restart) {
            /* this can happen if the entry got unlocked because of an async
             * entry was added to the head of the async queue. */
            GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
                "wakeup waiting for entry %p", entry);
            goto done;
          }

          GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
              "entry %p needs to be restarted", entry);
        } else {
          GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
              "entry %p unlocked after timeout", entry);
        }

        GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) entry);

        /* reschedule if gst_cond_wait_until returned early or we have to reschedule after
         * an unlock*/
        now = gst_clock_get_time (clock);
        mono_ts = g_get_monotonic_time ();

        GST_SYSTEM_CLOCK_ENTRY_LOCK ((GstClockEntryImpl *) entry);
        /* Might have been unscheduled in the meantime */
        status = GST_CLOCK_ENTRY_STATUS (entry);
        if (G_UNLIKELY (status == GST_CLOCK_UNSCHEDULED)) {
          goto done;
        }

        diff = GST_CLOCK_DIFF (now, entryt);

        if (diff <= CLOCK_MIN_WAIT_TIME) {
          /* timeout, this is fine, we can report success now */
          GST_CLOCK_ENTRY_STATUS (entry) = status = GST_CLOCK_OK;
          GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
              "entry %p finished, diff %" G_GINT64_FORMAT, entry, diff);

#ifdef WAIT_DEBUGGING
          final = gst_system_clock_get_internal_time (clock);
          GST_CAT_DEBUG (GST_CAT_CLOCK, "Waited for %" G_GINT64_FORMAT
              " got %" G_GINT64_FORMAT " diff %" G_GINT64_FORMAT
              " %g target-offset %" G_GINT64_FORMAT " %g", entryt, now,
              now - entryt,
              (double) (GstClockTimeDiff) (now - entryt) / GST_SECOND,
              (final - target),
              ((double) (GstClockTimeDiff) (final - target)) / GST_SECOND);
#endif
          goto done;
        } else {
          GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
              "entry %p restart, diff %" G_GINT64_FORMAT, entry, diff);
          /* we are going to poll again, set status back to busy */
          GST_CLOCK_ENTRY_STATUS (entry) = GST_CLOCK_BUSY;
        }
      }
    }
  } else {
    /* we are right on time or too late */
    if (G_UNLIKELY (diff == 0)) {
      GST_CLOCK_ENTRY_STATUS (entry) = status = GST_CLOCK_OK;
    } else {
      GST_CLOCK_ENTRY_STATUS (entry) = status = GST_CLOCK_EARLY;
    }
  }
done:
  return status;
}

static GstClockReturn
gst_system_clock_id_wait_jitter (GstClock * clock, GstClockEntry * entry,
    GstClockTimeDiff * jitter)
{
  GstClockReturn status;
  GstClockEntryImpl *entry_impl = (GstClockEntryImpl *) entry;

  GST_SYSTEM_CLOCK_LOCK (clock);
  ensure_entry_initialized (entry_impl);
  GST_SYSTEM_CLOCK_UNLOCK (clock);

  GST_SYSTEM_CLOCK_ENTRY_LOCK (entry_impl);
  status = GST_CLOCK_ENTRY_STATUS (entry);

  /* stop when we are unscheduled */
  if (G_UNLIKELY (status == GST_CLOCK_UNSCHEDULED)) {
    GST_SYSTEM_CLOCK_ENTRY_UNLOCK (entry_impl);
    return status;
  }

  if (G_UNLIKELY (status != GST_CLOCK_OK))
    GST_CAT_ERROR_OBJECT (GST_CAT_CLOCK, clock,
        "unexpected status %d for entry %p", status, entry);

  /* mark the entry as busy */
  GST_CLOCK_ENTRY_STATUS (entry) = GST_CLOCK_BUSY;

  GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "waiting on entry %p", entry);

  status =
      gst_system_clock_id_wait_jitter_unlocked (clock, entry, jitter, TRUE);

  GST_SYSTEM_CLOCK_ENTRY_UNLOCK (entry_impl);

  return status;
}

/* Start the async clock thread. Must be called with the object lock
 * held */
static gboolean
gst_system_clock_start_async (GstSystemClock * clock)
{
  GError *error = NULL;
  GstSystemClockPrivate *priv = clock->priv;

  if (G_LIKELY (priv->thread != NULL))
    return TRUE;                /* Thread already running. Nothing to do */

  priv->starting = TRUE;
  priv->thread = g_thread_try_new ("GstSystemClock",
      (GThreadFunc) gst_system_clock_async_thread, clock, &error);

  if (G_UNLIKELY (error))
    goto no_thread;

  /* wait for it to spin up */
  while (priv->starting)
    GST_SYSTEM_CLOCK_WAIT (clock);

  return TRUE;

  /* ERRORS */
no_thread:
  {
    priv->starting = FALSE;
    g_warning ("could not create async clock thread: %s", error->message);
    g_error_free (error);
  }
  return FALSE;
}

/* Add an entry to the list of pending async waits. The entry is inserted
 * in sorted order. If we inserted the entry at the head of the list, we
 * need to signal the thread as it might either be waiting on it or waiting
 * for a new entry.
 *
 * MT safe.
 */
static GstClockReturn
gst_system_clock_id_wait_async (GstClock * clock, GstClockEntry * entry)
{
  GstSystemClock *sysclock;
  GstSystemClockPrivate *priv;
  GstClockEntry *head;

  sysclock = GST_SYSTEM_CLOCK_CAST (clock);
  priv = sysclock->priv;

  GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "adding async entry %p", entry);

  GST_SYSTEM_CLOCK_LOCK (clock);
  /* Start the clock async thread if needed */
  if (G_UNLIKELY (!gst_system_clock_start_async (sysclock)))
    goto thread_error;

  ensure_entry_initialized ((GstClockEntryImpl *) entry);
  GST_SYSTEM_CLOCK_ENTRY_LOCK ((GstClockEntryImpl *) entry);
  if (G_UNLIKELY (GST_CLOCK_ENTRY_STATUS (entry) == GST_CLOCK_UNSCHEDULED))
    goto was_unscheduled;
  GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) entry);

  if (priv->entries)
    head = priv->entries->data;
  else
    head = NULL;

  /* need to take a ref */
  gst_clock_id_ref ((GstClockID) entry);

  /* insert the entry in sorted order */
  priv->entries = g_list_insert_sorted (priv->entries, entry,
      gst_clock_id_compare_func);

  /* only need to send the signal if the entry was added to the
   * front, else the thread is just waiting for another entry and
   * will get to this entry automatically. */
  if (priv->entries->data == entry) {
    GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
        "async entry added to head %p", head);
    if (head == NULL) {
      /* the list was empty before, signal the cond so that the async thread can
       * start taking a look at the queue */
      GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
          "first entry, sending signal");
      GST_SYSTEM_CLOCK_BROADCAST (clock);
    } else {
      GstClockReturn status;

      /* it was initialized before adding to the list */
      g_assert (((GstClockEntryImpl *) head)->initialized);

      GST_SYSTEM_CLOCK_ENTRY_LOCK ((GstClockEntryImpl *) head);
      status = GST_CLOCK_ENTRY_STATUS (head);
      GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "head entry %p status %d",
          head, status);

      if (status == GST_CLOCK_BUSY) {
        /* the async thread was waiting for an entry, unlock the wait so that it
         * looks at the new head entry instead, we only need to do this once */
        GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock,
            "head entry was busy. Wakeup async thread");
        GST_SYSTEM_CLOCK_ENTRY_BROADCAST ((GstClockEntryImpl *) head);
      }
      GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) head);
    }
  }
  GST_SYSTEM_CLOCK_UNLOCK (clock);

  return GST_CLOCK_OK;

  /* ERRORS */
thread_error:
  {
    /* Could not start the async clock thread */
    GST_SYSTEM_CLOCK_UNLOCK (clock);
    return GST_CLOCK_ERROR;
  }
was_unscheduled:
  {
    GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) entry);
    GST_SYSTEM_CLOCK_UNLOCK (clock);
    return GST_CLOCK_UNSCHEDULED;
  }
}

/* unschedule an entry. This will set the state of the entry to GST_CLOCK_UNSCHEDULED
 * and will signal any thread waiting for entries to recheck their entry.
 * We cannot really decide if the signal is needed or not because the entry
 * could be waited on in async or sync mode.
 *
 * MT safe.
 */
static void
gst_system_clock_id_unschedule (GstClock * clock, GstClockEntry * entry)
{
  GstClockReturn status;

  GST_SYSTEM_CLOCK_LOCK (clock);

  GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "unscheduling entry %p time %"
      GST_TIME_FORMAT, entry, GST_TIME_ARGS (GST_CLOCK_ENTRY_TIME (entry)));

  ensure_entry_initialized ((GstClockEntryImpl *) entry);

  GST_SYSTEM_CLOCK_ENTRY_LOCK ((GstClockEntryImpl *) entry);
  /* change the entry status to unscheduled */
  status = GST_CLOCK_ENTRY_STATUS (entry);
  GST_CLOCK_ENTRY_STATUS (entry) = GST_CLOCK_UNSCHEDULED;

  if (G_LIKELY (status == GST_CLOCK_BUSY)) {
    /* the entry was being busy, wake up the entry */
    GST_CAT_DEBUG_OBJECT (GST_CAT_CLOCK, clock, "entry was BUSY, doing wakeup");
    GST_SYSTEM_CLOCK_ENTRY_BROADCAST ((GstClockEntryImpl *) entry);
  }
  GST_SYSTEM_CLOCK_ENTRY_UNLOCK ((GstClockEntryImpl *) entry);
  GST_SYSTEM_CLOCK_UNLOCK (clock);
}
