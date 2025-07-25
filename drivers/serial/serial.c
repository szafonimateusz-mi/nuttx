/****************************************************************************
 * drivers/serial/serial.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <sys/types.h>
#include <sys/param.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <spawn.h>

#include <nuttx/irq.h>
#include <nuttx/ascii.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <nuttx/signal.h>
#include <nuttx/fs/fs.h>
#include <nuttx/cancelpt.h>
#include <nuttx/serial/serial.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/power/pm.h>
#include <nuttx/wqueue.h>
#include <nuttx/kthread.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Check watermark levels */

#if defined(CONFIG_SERIAL_IFLOWCONTROL) && \
    defined(CONFIG_SERIAL_IFLOWCONTROL_WATERMARKS)
#  if CONFIG_SERIAL_IFLOWCONTROL_LOWER_WATERMARK < 1
#    warning CONFIG_SERIAL_IFLOWCONTROL_LOWER_WATERMARK too small
#  endif
#  if CONFIG_SERIAL_IFLOWCONTROL_UPPER_WATERMARK > 99
#    warning CONFIG_SERIAL_IFLOWCONTROL_UPPER_WATERMARK too large
#  endif
#  if CONFIG_SERIAL_IFLOWCONTROL_LOWER_WATERMARK >= CONFIG_SERIAL_IFLOWCONTROL_UPPER_WATERMARK
#    warning CONFIG_SERIAL_IFLOWCONTROL_LOWER_WATERMARK too large
#    warning Must be less than CONFIG_SERIAL_IFLOWCONTROL_UPPER_WATERMARK
#  endif
#endif

/* Timing */

#define POLL_DELAY_USEC 1000

/****************************************************************************
 * Private Types
 ****************************************************************************/

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Poll support */

static void    uart_poll_notify(FAR uart_dev_t *dev, unsigned int min,
                                unsigned int max, pollevent_t eventset);

/* Write support */

static int     uart_putxmitchar(FAR uart_dev_t *dev, int ch,
                                bool oktoblock);
static inline ssize_t uart_irqwrite(FAR uart_dev_t *dev,
                                    FAR const char *buffer,
                                    size_t buflen);
static inline ssize_t uart_irqwritev(FAR uart_dev_t *dev,
                                     FAR struct uio *uio);
static int     uart_tcdrain(FAR uart_dev_t *dev,
                            bool cancelable, clock_t timeout);

static int     uart_tcsendbreak(FAR uart_dev_t *dev,
                                FAR struct file *filep,
                                unsigned int ms);

/* Character driver methods */

static int     uart_open(FAR struct file *filep);
static int     uart_close(FAR struct file *filep);
static ssize_t uart_readv(FAR struct file *filep, FAR struct uio *uio);
static ssize_t uart_writev(FAR struct file *filep, FAR struct uio *uio);
static int     uart_ioctl(FAR struct file *filep,
                          int cmd, unsigned long arg);
static int     uart_poll(FAR struct file *filep,
                         FAR struct pollfd *fds, bool setup);
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static int     uart_unlink(FAR struct inode *inode);
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_TTY_LAUNCH_ENTRY
/* Lanch program entry, this must be supplied by the application. */

int CONFIG_TTY_LAUNCH_ENTRYPOINT(int argc, FAR char *argv[]);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_serialops =
{
  uart_open,    /* open */
  uart_close,   /* close */
  NULL,         /* read */
  NULL,         /* write */
  NULL,         /* seek */
  uart_ioctl,   /* ioctl */
  NULL,         /* mmap */
  NULL,         /* truncate */
  uart_poll,    /* poll */
  uart_readv,   /* readv */
  uart_writev   /* writev */
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  , uart_unlink /* unlink */
#endif
};

#ifdef CONFIG_TTY_LAUNCH
static struct work_s g_serial_work;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: uart_is_termios_hw_change
 *
 * Description:
 *   Return true if the termios hw change
 *
 ****************************************************************************/

static bool uart_is_termios_hw_change(FAR struct file *filep,
                                      FAR const struct termios *new)
{
  FAR struct inode *inode = filep->f_inode;
  FAR uart_dev_t *dev = inode->i_private;
  struct termios old;
  int ret;

  if (new == NULL)
    {
      return false;
    }

  memset(&old, 0, sizeof(old));
  ret = dev->ops->ioctl(filep, TCGETS, (unsigned long)(uintptr_t)&old);
  if (ret >= 0)
    {
      if (old.c_speed != new->c_speed)
        {
          return true;
        }

      if ((old.c_cflag ^ new->c_cflag) & ~(HUPCL | CREAD | CLOCAL))
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Name: uart_poll_notify
 ****************************************************************************/

static void uart_poll_notify(FAR uart_dev_t *dev, unsigned int min,
                             unsigned int max, pollevent_t eventset)
{
  irqstate_t flags;

  DEBUGASSERT(max > min && max - min <= CONFIG_SERIAL_NPOLLWAITERS);

  flags = enter_critical_section();
  sched_lock();

  /* Notify the fds in range dev->fds[min] - dev->fds[max] */

  poll_notify(&dev->fds[min], max - min, eventset);

  sched_unlock();
  leave_critical_section(flags);
}

/****************************************************************************
 * Name: uart_putxmitchar
 ****************************************************************************/

static int uart_putxmitchar(FAR uart_dev_t *dev, int ch, bool oktoblock)
{
  irqstate_t flags;
  int nexthead;
  int ret;

  /* Loop until we are able to add the character to the TX buffer. */

  for (; ; )
    {
      /* Increment to see what the next head pointer will be.
       * We need to use the "next" head pointer to determine when the
       * circular buffer would overrun
       */

      nexthead = dev->xmit.head + 1;
      if (nexthead >= dev->xmit.size)
        {
          nexthead = 0;
        }

      /* Check if the TX buffer is full */

      if (nexthead != dev->xmit.tail)
        {
          /* No.. not full.  Add the character to the TX buffer and return. */

          dev->xmit.buffer[dev->xmit.head] = ch;
          dev->xmit.head = nexthead;
          break;
        }

      /* The TX buffer is full.  Should be block, waiting for the hardware
       * to remove some data from the TX buffer?
       */

      else if (oktoblock)
        {
          /* The following steps must be atomic with respect to serial
           * interrupt handling.
           *
           * This critical section is also used for the serialization
           * with the up_putc-based syslog channels.
           * See https://github.com/apache/nuttx/issues/14662
           */

          flags = enter_critical_section();

          /* Check again...  In certain race conditions an interrupt may
           * have occurred between the test at the top of the loop and
           * entering the critical section and the TX buffer may no longer
           * be full.
           *
           * NOTE: On certain devices, such as USB CDC/ACM, the entire TX
           * buffer may have been emptied in this race condition.  In that
           * case, the logic would hang below waiting for space in the TX
           * buffer without this test.
           */

          if (nexthead != dev->xmit.tail)
            {
              ret = OK;
            }

#ifdef CONFIG_SERIAL_REMOVABLE
          /* Check if the removable device is no longer connected while we
           * have interrupts off.  We do not want the transition to occur
           * as a race condition before we begin the wait.
           */

          else if (dev->disconnected)
            {
              ret = -ENOTCONN;
            }
#endif
          else
            {
              /* Wait for some characters to be sent from the buffer with
               * the TX interrupt enabled.  When the TX interrupt is enabled,
               * uart_xmitchars() should execute and remove some of the data
               * from the TX buffer.
               *
               * NOTE that interrupts will be re-enabled while we wait for
               * the semaphore.
               */

#ifdef CONFIG_SERIAL_TXDMA
              uart_dmatxavail(dev);
#endif
              uart_enabletxint(dev);
              ret = nxsem_wait(&dev->xmitsem);
              uart_disabletxint(dev);
            }

          leave_critical_section(flags);

#ifdef CONFIG_SERIAL_REMOVABLE
          /* Check if the removable device was disconnected while we were
           * waiting.
           */

          if (dev->disconnected)
            {
              return -ENOTCONN;
            }
#endif

          /* Check if we were awakened by signal. */

          if (ret < 0)
            {
              /* A signal received while waiting for the xmit buffer to
               * become non-full will abort the transfer.
               */

              return -EINTR;
            }
        }

      /* The caller has request that we not block for data.  So return the
       * EAGAIN error to signal this situation.
       */

      else
        {
          return -EAGAIN;
        }
    }

  /* We won't get here.  Some compilers may complain that this code is
   * unreachable.
   */

  return OK;
}

/****************************************************************************
 * Name: uart_putc
 ****************************************************************************/

static inline void uart_putchars(FAR uart_dev_t *dev,
                                 FAR const void *buf, size_t len)
{
  FAR const char *pbuf = buf;

  while (len > 0)
    {
      while (!uart_txready(dev))
        {
        }

      if (dev->ops->sendbuf)
        {
          ssize_t ret = uart_sendbuf(dev, pbuf, len);
          if (ret > 0)
            {
              pbuf += ret;
              len -= ret;
            }
        }
      else
        {
          uart_send(dev, *pbuf++);
          len--;
        }
    }
}

/****************************************************************************
 * Name: uart_irqwrite
 ****************************************************************************/

static inline ssize_t uart_irqwrite(FAR uart_dev_t *dev,
                                    FAR const char *buffer,
                                    size_t buflen)
{
  size_t tail = 0;
  size_t head = buflen;

  /* Do output post-processing */

  if ((dev->tc_oflag & OPOST) != 0)
    {
      for (head = 0; head < buflen; head++)
        {
          int ch = buffer[head];

          /* Mapping CR to NL? */

          if ((ch == '\r') && (dev->tc_oflag & OCRNL) != 0)
            {
              uart_putchars(dev, &buffer[tail], head - tail);
              uart_putchars(dev, "\n", 1);
              tail = head + 1;
            }

          /* Are we interested in newline processing? */

          if ((ch == '\n') && (dev->tc_oflag & (ONLCR | ONLRET)) != 0)
            {
              uart_putchars(dev, &buffer[tail], head - tail);
              uart_putchars(dev, "\r", 1);
              tail = head;
            }
        }
    }

  /* Output the character, using the low-level direct UART interfaces */

  uart_putchars(dev, &buffer[tail], head - tail);
  return buflen;
}

/****************************************************************************
 * Name: uart_irqwritev
 ****************************************************************************/

static inline ssize_t uart_irqwritev(FAR uart_dev_t *dev,
                                     FAR struct uio *uio)
{
  ssize_t error = 0;
  ssize_t total = 0;
  int iovcnt = uio->uio_iovcnt;
  int i;

  for (i = 0; i < iovcnt; i++)
    {
      const struct iovec *iov = &uio->uio_iov[i];
      if (iov->iov_len == 0)
        {
          continue;
        }

      ssize_t written = uart_irqwrite(dev, iov->iov_base, iov->iov_len);
      if (written < 0)
        {
          error = written;
          break;
        }

      if (SSIZE_MAX - total < written)
        {
          error = -EOVERFLOW;
          break;
        }

      total += written;
    }

  if (error != 0 && total == 0)
    {
      return error;
    }

  return total;
}

/****************************************************************************
 * Name: uart_tcdrain
 *
 * Description:
 *   Block further TX input.
 *   Wait until all data has been transferred from the TX buffer and
 *   until the hardware TX FIFOs are empty.
 *
 ****************************************************************************/

static int uart_tcdrain(FAR uart_dev_t *dev,
                        bool cancelable, clock_t timeout)
{
  int ret;

  /* tcdrain is a cancellation point */

  if (cancelable && enter_cancellation_point())
    {
#ifdef CONFIG_CANCELLATION_POINTS
      /* If there is a pending cancellation, then do not perform
       * the wait.  Exit now with ECANCELED.
       */

      leave_cancellation_point();
      return -ECANCELED;
#endif
    }

  /* Get exclusive access to the to dev->xmit.  We cannot permit new data to
   * be written while we are trying to flush the old data.
   *
   * A signal received while waiting for access to the xmit.head will abort
   * the operation with EINTR.
   */

  ret = nxmutex_lock(&dev->xmit.lock);
  if (ret >= 0)
    {
      irqstate_t flags;
      clock_t start;

      /* Trigger emission to flush the contents of the tx buffer */

      flags = enter_critical_section();

#ifdef CONFIG_SERIAL_REMOVABLE
      /* Check if the removable device is no longer connected while we have
       * interrupts off.  We do not want the transition to occur as a race
       * condition before we begin the wait.
       */

      if (dev->disconnected)
        {
          dev->xmit.tail = dev->xmit.head;  /* Drop the buffered TX data */
          ret = -ENOTCONN;
        }
      else
#endif
        {
          /* Continue waiting while the TX buffer is not empty.
           *
           * NOTE: There is no timeout on the following loop.  In
           * situations were this loop could hang (with hardware flow
           * control, as an example),  the caller should call
           * tcflush() first to discard this buffered Tx data.
           */

          ret = OK;
          while (ret >= 0 && dev->xmit.head != dev->xmit.tail)
            {
              /* Wait for some characters to be sent from the buffer with
               * the TX interrupt enabled.  When the TX interrupt is
               * enabled, uart_xmitchars() should execute and remove some
               * of the data from the TX buffer.  We may have to wait several
               * times for the TX buffer to be entirely emptied.
               *
               * NOTE that interrupts will be re-enabled while we wait for
               * the semaphore.
               */

#ifdef CONFIG_SERIAL_TXDMA
              uart_dmatxavail(dev);
#endif
              uart_enabletxint(dev);
              ret = nxsem_wait(&dev->xmitsem);
              uart_disabletxint(dev);
            }
        }

      leave_critical_section(flags);

      /* The TX buffer is empty (or an error occurred).  But there still may
       * be data in the UART TX FIFO.  We get no asynchronous indication of
       * this event, so we have to do a busy wait poll.
       */

      /* Set up for the timeout
       *
       * REVISIT:  This is a kludge.  The correct fix would be add an
       * interface to the lower half driver so that the tcflush() operation
       * all also cause the lower half driver to clear and reset the Tx FIFO.
       */

      start = clock_systime_ticks();

      if (ret >= 0)
        {
          while (!uart_txempty(dev))
            {
              clock_t elapsed;

              nxsig_usleep(POLL_DELAY_USEC);

              /* Check for a timeout */

              elapsed = clock_systime_ticks() - start;
              if (elapsed >= timeout)
                {
                  nxmutex_unlock(&dev->xmit.lock);
                  return -ETIMEDOUT;
                }
            }
        }

      nxmutex_unlock(&dev->xmit.lock);
    }

  if (cancelable)
    {
      leave_cancellation_point();
    }

  return ret;
}

/****************************************************************************
 * Name: uart_tcsendbreak
 *
 * Description:
 *   Request a serial line Break by calling the lower half driver's
 *   BSD-compatible Break IOCTLs TIOCSBRK and TIOCCBRK, with a sleep of the
 *   specified duration between them.
 *
 * Input Parameters:
 *   dev      - Serial device.
 *   filep    - Required for issuing lower half driver IOCTL call.
 *   ms       - If non-zero, duration of the Break in milliseconds; if
 *              zero, duration is 400 milliseconds.
 *
 * Returned Value:
 *   0 on success or a negated errno value on failure.
 *
 ****************************************************************************/

static int uart_tcsendbreak(FAR uart_dev_t *dev, FAR struct file *filep,
                            unsigned int ms)
{
  int ret;

  /* REVISIT: Do we need to perform the equivalent of tcdrain() before
   * beginning the Break to avoid corrupting the transmit data? If so, note
   * that just calling uart_tcdrain() here would create a race condition,
   * since new transmit data could be written after uart_tcdrain() returns
   * but before we re-acquire the dev->xmit.lock here. Therefore, we would
   * need to refactor the functional portion of uart_tcdrain() to a separate
   * function and call it from both uart_tcdrain() and uart_tcsendbreak()
   * in critical section and with xmit lock already held.
   */

  if (dev->ops->ioctl)
    {
      ret = nxmutex_lock(&dev->xmit.lock);
      if (ret >= 0)
        {
          /* Request lower half driver to start the Break */

          ret = dev->ops->ioctl(filep, TIOCSBRK, 0);
          if (ret >= 0)
            {
              /* Wait 400 ms or the requested Break duration */

              nxsig_usleep((ms == 0) ? 400000 : ms * 1000);

              /* Request lower half driver to end the Break */

              ret = dev->ops->ioctl(filep, TIOCCBRK, 0);
            }
        }

      nxmutex_unlock(&dev->xmit.lock);
    }
  else
    {
      /* With no lower half IOCTL, we cannot request Break at all. */

      ret = -ENOTTY;
    }

  return ret;
}

/****************************************************************************
 * Name: uart_open
 *
 * Description:
 *   This routine is called whenever a serial port is opened.
 *
 ****************************************************************************/

static int uart_open(FAR struct file *filep)
{
  FAR struct inode *inode = filep->f_inode;
  FAR uart_dev_t   *dev   = inode->i_private;
  uint8_t           tmp;
  int               ret;

  /* If the port is the middle of closing, wait until the close is finished.
   * If a signal is received while we are waiting, then return EINTR.
   */

  ret = nxmutex_lock(&dev->closelock);
  if (ret < 0)
    {
      /* A signal received while waiting for the last close operation. */

      return ret;
    }

#ifdef CONFIG_SERIAL_REMOVABLE
  /* If the removable device is no longer connected, refuse to open the
   * device.  We check this after obtaining the close semaphore because
   * we might have been waiting when the device was disconnected.
   */

  if (dev->disconnected)
    {
      ret = -ENOTCONN;
      goto errout_with_lock;
    }
#endif

  /* Start up serial port */

  /* Increment the count of references to the device. */

  tmp = dev->open_count + 1;
  if (tmp == 0)
    {
      /* More than 255 opens; uint8_t overflows to zero */

      ret = -EMFILE;
      goto errout_with_lock;
    }

  /* Check if this is the first time that the driver has been opened. */

  if (tmp == 1)
    {
      irqstate_t flags = enter_critical_section();

      /* If this is the console, then the UART has already been
       * initialized.
       */

      if (!dev->isconsole)
        {
          /* Perform one time hardware initialization */

          ret = uart_setup(dev);
          if (ret < 0)
            {
              leave_critical_section(flags);
              goto errout_with_lock;
            }
        }

      /* In any event, we do have to configure for interrupt driven mode of
       * operation.  Attach the hardware IRQ(s). Hmm.. should shutdown() the
       * the device in the rare case that uart_attach() fails, tmp==1, and
       * this is not the console.
       */

      ret = uart_attach(dev);
      if (ret < 0)
        {
          if (!dev->isconsole)
            {
              uart_shutdown(dev);
            }

          leave_critical_section(flags);
          goto errout_with_lock;
        }

#ifdef CONFIG_SERIAL_RXDMA
      /* Notify DMA that there is free space in the RX buffer */

      uart_dmarxfree(dev);
#endif

      /* Enable the RX interrupt */

      uart_enablerxint(dev);
      leave_critical_section(flags);
    }

  /* Save the new open count on success */

  dev->open_count = tmp;

errout_with_lock:
  nxmutex_unlock(&dev->closelock);
  return ret;
}

/****************************************************************************
 * Name: uart_close
 *
 * Description:
 *   This routine is called when the serial port gets closed.
 *   It waits for the last remaining data to be sent.
 *
 ****************************************************************************/

static int uart_close(FAR struct file *filep)
{
  FAR struct inode *inode = filep->f_inode;
  FAR uart_dev_t   *dev   = inode->i_private;
  irqstate_t        flags;

  /* Get exclusive access to the close semaphore (to synchronize open/close
   * operations.
   * NOTE: that we do not let this wait be interrupted by a signal.
   * Technically, we should, but almost no one every checks the return value
   * from close() so we avoid a potential memory leak by ignoring signals in
   * this case.
   */

  nxmutex_lock(&dev->closelock);
  if (dev->open_count > 1)
    {
      dev->open_count--;
      nxmutex_unlock(&dev->closelock);
      return OK;
    }

  /* There are no more references to the port */

  dev->open_count = 0;

  /* Stop accepting input */

  uart_disablerxint(dev);

  /* Prevent blocking if the device is opened with O_NONBLOCK */

  if ((filep->f_oflags & O_NONBLOCK) == 0)
    {
      /* Now we wait for the transmit buffer(s) to clear */

      uart_tcdrain(dev, false, 4 * TICK_PER_SEC);
    }

  /* Free the IRQ and disable the UART */

  flags = enter_critical_section();  /* Disable interrupts */
  uart_detach(dev);                  /* Detach interrupts */

  /* Check for the serial console UART */

  if (!dev->isconsole)
    {
      uart_shutdown(dev);            /* Disable the UART */
    }

  leave_critical_section(flags);

  /* Wake up read and poll functions */

  uart_datareceived(dev);

  /* We need to re-initialize the semaphores if this is the last close
   * of the device, as the close might be caused by pthread_cancel() of
   * a thread currently blocking on any of them.
   */

  uart_reset_sem(dev);

  if (dev->unlinked)
    {
      nxmutex_unlock(&dev->closelock);
      nxmutex_destroy(&dev->xmit.lock);
      nxmutex_destroy(&dev->recv.lock);
      nxmutex_destroy(&dev->closelock);
      nxsem_destroy(&dev->xmitsem);
      nxsem_destroy(&dev->recvsem);
      uart_release(dev);
      return OK;
    }

  nxmutex_unlock(&dev->closelock);
  return OK;
}

/****************************************************************************
 * Name: uart_readv
 ****************************************************************************/

static ssize_t uart_readv(FAR struct file *filep, FAR struct uio *uio)
{
  FAR struct inode *inode = filep->f_inode;
  FAR uart_dev_t *dev = inode->i_private;
  FAR struct uart_buffer_s *rxbuf = &dev->recv;
#ifdef CONFIG_SERIAL_IFLOWCONTROL_WATERMARKS
  unsigned int nbuffered;
  unsigned int watermark;
  sbuf_size_t head;
#endif
  irqstate_t flags;
  ssize_t recvd = 0;
  ssize_t buflen;
  bool echoed = false;
  sbuf_size_t tail;
  char ch;
  int ret;

  /* Only one user can access rxbuf->tail at a time */

  ret = nxmutex_lock(&dev->recv.lock);
  if (ret < 0)
    {
      /* A signal received while waiting for access to the recv.tail will
       * abort the transfer.  After the transfer has started, we are
       * committed and signals will be ignored.
       */

      return ret;
    }

  /* Loop while we still have data to copy to the receive buffer.
   * we add data to the head of the buffer; uart_xmitchars takes the
   * data from the end of the buffer.
   */

  buflen = uio->uio_resid;
  while (recvd < buflen)
    {
#ifdef CONFIG_SERIAL_REMOVABLE
      /* If the removable device is no longer connected, refuse to read any
       * further from the device.
       */

      if (dev->disconnected)
        {
          if (recvd == 0)
            {
              recvd = -ENOTCONN;
            }

          break;
        }
#endif

      /* Check if there is more data to return in the circular buffer.
       * NOTE: Rx interrupt handling logic may asynchronously increment
       * the head index but must not modify the tail index.  The tail
       * index is only modified in this function.  Therefore, no
       * special handshaking is required here.
       *
       * The head and tail pointers values are sized based
       * on the architecture. If the architecture reads 16-bit values
       * atomically by nature, they are 16-bit values. On architectures
       * where 16-bit access is split into two non-atomic 8-bit accesses,
       * the pointers are 8-bit.
       *
       * The following code is therefore safe even with interrupts enabled.
       */

      tail = rxbuf->tail;
      if (rxbuf->head != tail)
        {
          /* Take the next character from the tail of the buffer */

          ch = rxbuf->buffer[tail];

          /* Increment the tail index.  Most operations are done using the
           * local variable 'tail' so that the final rxbuf->tail update
           * is atomic.
           */

          if (++tail >= rxbuf->size)
            {
              tail = 0;
            }

          rxbuf->tail = tail;

          /* Do input processing if any is enabled */

          if (dev->tc_iflag & (INLCR | IGNCR | ICRNL))
            {
              /* \n -> \r or \r -> \n translation? */

              if ((ch == '\n') && (dev->tc_iflag & INLCR))
                {
                  ch = '\r';
                }
              else if ((ch == '\r') && (dev->tc_iflag & ICRNL))
                {
                  ch = '\n';
                }

              /* Discarding \r ? */

              if ((ch == '\r') && (dev->tc_iflag & IGNCR))
                {
                  continue;
                }
            }

          if ((dev->tc_lflag & ICANON) &&
              (ch == ASCII_BS || ch == ASCII_DEL))
            {
              if (recvd > 0)
                {
                  static const char zero = '\0';
                  uio_copyfrom(uio, recvd, &zero, 1);
                  recvd--;
                  if (dev->tc_lflag & ECHO)
                    {
                      uart_putxmitchar(dev, '\b', true);
                      uart_putxmitchar(dev, ' ', true);
                      uart_putxmitchar(dev, '\b', true);

#ifdef CONFIG_SERIAL_TXDMA
                      uart_dmatxavail(dev);
#endif
                      uart_enabletxint(dev);
                    }
                }

                continue;
            }

          /* Specifically not handled:
           *
           * All of the local modes; echo, line editing, etc.
           * Anything to do with break or parity errors.
           * ISTRIP - we should be 8-bit clean.
           * IUCLC - Not Posix
           * IXON/OXOFF - no xon/xoff flow control.
           */

          /* Store the received character */

          uio_copyfrom(uio, recvd, &ch, 1);
          recvd++;

          if (dev->tc_lflag & ECHO)
            {
              /* Check for the beginning of a VT100 escape sequence, 3 byte */

              if (ch == ASCII_ESC)
                {
                  /* Mark that we should skip 2 more bytes */

                  dev->escape = 2;
                  continue;
                }
              else if (dev->escape == 2 && ch != ASCII_LBRACKET)
                {
                  /* It's not an <esc>[x 3 byte sequence, show it */

                  dev->escape = 0;
                }
              else if (dev->escape > 0)
                {
                  /* Skipping character count down */

                  dev->escape--;
                  continue;
                }

              /* Echo if the character is not a control byte */

              if (!iscntrl(ch & 0xff) || ch == '\n')
                {
                  if (ch == '\n')
                    {
                      uart_putxmitchar(dev, '\r', true);
                    }

                  uart_putxmitchar(dev, ch, true);

                  /* Mark the tx buffer have echoed content here,
                   * to avoid the tx buffer is empty such as special escape
                   * sequence received, but enable the tx interrupt.
                   */

                  if (dev->tc_lflag & ICANON)
                    {
#ifdef CONFIG_SERIAL_TXDMA
                      uart_dmatxavail(dev);
#endif
                      uart_enabletxint(dev);
                    }
                  else
                    {
                      echoed = true;
                    }
                }
            }

          if ((dev->tc_lflag & ICANON) && ch == '\n')
            {
              break;
            }
        }

      /* Otherwise we are going to have to wait for data to arrive */

      else
        {
          /* Disable all interrupts and test again... */

          flags = enter_critical_section();

          /* Disable Rx interrupts and test again... */

          uart_disablerxint(dev);

          /* If the Rx ring buffer still empty?  Bytes may have been added
           * between the last time that we checked and when we disabled
           * interrupts.
           */

          if (rxbuf->head == rxbuf->tail)
            {
              /* Yes.. the buffer is still empty.  We will need to wait for
               * additional data to be received.
               */

#ifdef CONFIG_SERIAL_RXDMA
              /* Notify DMA that there is free space in the RX buffer */

              uart_dmarxfree(dev);
#endif
              /* Wait with the RX interrupt re-enabled.  All interrupts are
               * disabled briefly to assure that the following operations
               * are atomic.
               */

              /* Re-enable UART Rx interrupts */

              uart_enablerxint(dev);

              /* Check again if the RX buffer is empty.  The UART driver
               * might have buffered data received between disabling the
               * RX interrupt and entering the critical section.  Some
               * drivers (looking at you, cdcacm...) will push the buffer
               * to the receive queue during uart_enablerxint().
               * Just continue processing the RX queue if this happens.
               */

              if (rxbuf->head != rxbuf->tail)
                {
                  leave_critical_section(flags);
                  continue;
                }

#ifdef CONFIG_DEV_SERIAL_FULLBLOCKS
              /* No... then we would have to wait to get receive more data.
               * If the user has specified the O_NONBLOCK option, then just
               * return what we have.
               */

              else if ((filep->f_oflags & O_NONBLOCK) != 0)
                {
                  /* If nothing was transferred, then return the -EAGAIN
                   * error (not zero which means end of file).
                   */

                  if (recvd < 1)
                    {
                      recvd = -EAGAIN;
                    }

                  leave_critical_section(flags);
                  break;
                }
#else
              /* No... the circular buffer is empty.  Have we returned
               * anything to the caller?
               */

              else if (recvd > 0 && !(dev->tc_lflag & ICANON))
                {
                  /* Yes.. break out of the loop and return the number
                   * of bytes received up to the wait condition.
                   */

                  leave_critical_section(flags);
                  break;
                }

              else if (filep->f_inode == 0)
                {
                  /* File has been closed.
                   * Descriptor is not valid.
                   */

                  recvd = -EBADFD;
                  leave_critical_section(flags);
                  break;
                }

              /* No... then we would have to wait to get receive some data.
               * If the user has specified the O_NONBLOCK option, then do not
               * wait.
               */

              else if ((filep->f_oflags & O_NONBLOCK) != 0)
                {
                  /* Break out of the loop returning -EAGAIN */

                  recvd = -EAGAIN;
                  leave_critical_section(flags);
                  break;
                }
#endif

#ifdef CONFIG_SERIAL_REMOVABLE
              /* Check again if the removable device is still connected
               * while we have interrupts off.  We do not want the transition
               * to occur as a race condition before we begin the wait.
               */

              if (dev->disconnected)
                {
                  ret = -ENOTCONN;
                }
              else
#endif
                {
                  /* Now wait with the Rx interrupt enabled.  NuttX will
                   * automatically re-enable global interrupts when this
                   * thread goes to sleep.
                   */

                  if (dev->tc_lflag & ICANON)
                    {
#ifdef CONFIG_SERIAL_TERMIOS
                      dev->minrecv = 0;
#endif
                      nxmutex_unlock(&dev->recv.lock);
                      ret = nxsem_wait(&dev->recvsem);
                      nxmutex_lock(&dev->recv.lock);
                    }
                  else
                    {
#ifdef CONFIG_SERIAL_TERMIOS
                      dev->minrecv = MIN(buflen - recvd,
                                         dev->minread - recvd);
                      if (dev->timeout)
                        {
                          nxmutex_unlock(&dev->recv.lock);
                          ret = nxsem_tickwait(&dev->recvsem,
                                              DSEC2TICK(dev->timeout));
                        }
                      else
#endif
                        {
                          nxmutex_unlock(&dev->recv.lock);
                          ret = nxsem_wait(&dev->recvsem);
                        }

                      nxmutex_lock(&dev->recv.lock);

#ifdef CONFIG_SERIAL_TERMIOS
                      dev->minrecv = dev->minread;
#endif
                    }
                }

              leave_critical_section(flags);

              /* Was a signal received while waiting for data to be
               * received?  Was a removable device disconnected while
               * we were waiting?
               */

#ifdef CONFIG_SERIAL_REMOVABLE
              if (ret < 0 || dev->disconnected)
#else
              if (ret < 0)
#endif
                {
                  /* POSIX requires that we return after a signal is
                   * received.
                   * If some bytes were read, we need to return the
                   * number of bytes read; if no bytes were read, we
                   * need to return -1 with the errno set correctly.
                   */

                  if (recvd == 0)
                    {
                      /* No bytes were read, return -EINTR
                       * (the VFS layer will set the errno value
                       * appropriately).
                       */

#ifdef CONFIG_SERIAL_REMOVABLE
                      recvd = dev->disconnected ? -ENOTCONN : ret;
#else
                      recvd = ret;
#endif
                    }

                  break;
                }
            }
          else
            {
              /* No... the ring buffer is no longer empty.  Just re-enable Rx
               * interrupts and accept the new data on the next time through
               * the loop.
               */

              leave_critical_section(flags);

              uart_enablerxint(dev);
            }
        }
    }

  if (echoed)
    {
#ifdef CONFIG_SERIAL_TXDMA
      uart_dmatxavail(dev);
#endif
      uart_enabletxint(dev);
    }

#ifdef CONFIG_SERIAL_RXDMA
  /* Notify DMA that there is free space in the RX buffer */

  flags = enter_critical_section();
  uart_dmarxfree(dev);
  leave_critical_section(flags);
#endif

  /* RX interrupt could be disabled by RX buffer overflow. Enable it now. */

  uart_enablerxint(dev);

#ifdef CONFIG_SERIAL_IFLOWCONTROL
#ifdef CONFIG_SERIAL_IFLOWCONTROL_WATERMARKS
  /* How many bytes are now buffered. Head needs to be copied
   * to a non-volatile variable to prevent TOCTOU error in case
   * the interrupt handler changes it between comparison and assignment.
   * (Copy of tail is not strictly needed but saves us few instructions.)
   */

  rxbuf = &dev->recv;
  head = rxbuf->head;
  tail = rxbuf->tail;

  if (head >= tail)
    {
      nbuffered = head - tail;
    }
  else
    {
      nbuffered = rxbuf->size - tail + head;
    }

  /* Is the level now below the watermark level that we need to report? */

  watermark = (CONFIG_SERIAL_IFLOWCONTROL_LOWER_WATERMARK *
               rxbuf->size) / 100;
  if (nbuffered <= watermark)
    {
      /* Let the lower level driver know that the watermark level has been
       * crossed.  It will probably deactivate RX flow control.
       */

      uart_rxflowcontrol(dev, nbuffered, false);
    }
#else
  /* Is the RX buffer empty? */

  if (rxbuf->head == rxbuf->tail)
    {
      /* Deactivate RX flow control. */

      uart_rxflowcontrol(dev, 0, false);
    }
#endif
#endif

  nxmutex_unlock(&dev->recv.lock);
  if (recvd >= 0)
    {
      uio_advance(uio, recvd);
    }

  return recvd;
}

/****************************************************************************
 * Name: uart_writev
 ****************************************************************************/

static ssize_t uart_writev(FAR struct file *filep, FAR struct uio *uio)
{
  FAR struct inode *inode    = filep->f_inode;
  FAR uart_dev_t   *dev      = inode->i_private;
  ssize_t           nwritten;
  ssize_t           buflen;
  bool              oktoblock;
  int               ret;
  char              ch;

  /* We may receive serial writes through this path from interrupt handlers
   * and from debug output in the IDLE task!  In these cases, we will need to
   * do things a little differently.
   */

  if (up_interrupt_context() || sched_idletask())
    {
      irqstate_t flags;

#ifdef CONFIG_SERIAL_REMOVABLE
      /* If the removable device is no longer connected, refuse to write to
       * the device.
       */

      if (dev->disconnected)
        {
          return -ENOTCONN;
        }
#endif

      flags = enter_critical_section();
      ret = uart_irqwritev(dev, uio);
      leave_critical_section(flags);

      return ret;
    }

  buflen = nwritten = uio->uio_resid;

  /* Only one user can access dev->xmit.head at a time */

  ret = nxmutex_lock(&dev->xmit.lock);
  if (ret < 0)
    {
      /* A signal received while waiting for access to the xmit.head will
       * abort the transfer.  After the transfer has started, we are
       * committed and signals will be ignored.
       */

      return ret;
    }

#ifdef CONFIG_SERIAL_REMOVABLE
  /* If the removable device is no longer connected, refuse to write to the
   * device.  This check occurs after taking the xmit.lock because the
   * disconnection event might have occurred while we were waiting for
   * access to the transmit buffers.
   */

  if (dev->disconnected)
    {
      nxmutex_unlock(&dev->xmit.lock);
      return -ENOTCONN;
    }
#endif

  /* Can the following loop block, waiting for space in the TX
   * buffer?
   */

  oktoblock = ((filep->f_oflags & O_NONBLOCK) == 0);

  /* Loop while we still have data to copy to the transmit buffer.
   * we add data to the head of the buffer; uart_xmitchars takes the
   * data from the end of the buffer.
   */

  uart_disabletxint(dev);
  for (; buflen; uio_advance(uio, 1), buflen--)
    {
      uio_copyto(uio, 0, &ch, 1);
      ret = OK;

      /* Do output post-processing */

      if ((dev->tc_oflag & OPOST) != 0)
        {
          /* Mapping CR to NL? */

          if ((ch == '\r') && (dev->tc_oflag & OCRNL) != 0)
            {
              ch = '\n';
            }

          /* Are we interested in newline processing? */

          if ((ch == '\n') && (dev->tc_oflag & (ONLCR | ONLRET)) != 0)
            {
              ret = uart_putxmitchar(dev, '\r', oktoblock);
            }

          /* Specifically not handled:
           *
           * OXTABS - primarily a full-screen terminal optimization
           * ONOEOT - Unix interoperability hack
           * OLCUC  - Not specified by POSIX
           * ONOCR  - low-speed interactive optimization
           */
        }

      /* Put the character into the transmit buffer */

      if (ret >= 0)
        {
          ret = uart_putxmitchar(dev, ch, oktoblock);
        }

      /* uart_putxmitchar() might return an error under one of two
       * conditions:  (1) The wait for buffer space might have been
       * interrupted by a signal (ret should be -EINTR), (2) if
       * CONFIG_SERIAL_REMOVABLE is defined, then uart_putxmitchar()
       * might also return if the serial device was disconnected
       * (with -ENOTCONN), or (3) if O_NONBLOCK is specified, then
       * then uart_putxmitchar() might return -EAGAIN if the output
       * TX buffer is full.
       */

      if (ret < 0)
        {
          /* POSIX requires that we return -1 and errno set if no data was
           * transferred.  Otherwise, we return the number of bytes in the
           * interrupted transfer.
           */

          if (buflen < (size_t)nwritten)
            {
              /* Some data was transferred.  Return the number of bytes that
               * were successfully transferred.
               */

              nwritten -= buflen;
            }
          else
            {
              /* No data was transferred. Return the negated errno value.
               * The VFS layer will set the errno value appropriately).
               */

              nwritten = ret;
            }

          break;
        }
    }

  if (dev->xmit.head != dev->xmit.tail)
    {
#ifdef CONFIG_SERIAL_TXDMA
      uart_dmatxavail(dev);
#endif
      uart_enabletxint(dev);
    }

  nxmutex_unlock(&dev->xmit.lock);
  return nwritten;
}

/****************************************************************************
 * Name: uart_ioctl
 ****************************************************************************/

static int uart_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  FAR struct inode *inode = filep->f_inode;
  FAR uart_dev_t   *dev   = inode->i_private;
  FAR struct termios *termiosp = (FAR struct termios *)(uintptr_t)arg;
  int ret = -ENOTTY;

  /* Handle TTY-level IOCTLs here */

  /* Let low-level driver handle the call first,
   * but skip TCSETS if no hardware change.
   */

  if (dev->ops->ioctl && (cmd != TCSETS ||
      uart_is_termios_hw_change(filep, termiosp)))
    {
      ret = dev->ops->ioctl(filep, cmd, arg);
    }

  /* The device ioctl() handler returns -ENOTTY when it doesn't know
   * how to handle the command. Check if we can handle it here.
   */

  if (ret == -ENOTTY)
    {
      switch (cmd)
        {
          /* Get the number of bytes that may be read from the RX buffer
           * (without waiting)
           */

          case FIONREAD:
            {
              int count;
              irqstate_t flags = enter_critical_section();

              /* Determine the number of bytes available in the RX buffer */

              if (dev->recv.tail <= dev->recv.head)
                {
                  count = dev->recv.head - dev->recv.tail;
                }
              else
                {
                  count = dev->recv.size - (dev->recv.tail - dev->recv.head);
                }

              leave_critical_section(flags);

              *(FAR int *)((uintptr_t)arg) = count;
              ret = 0;
            }
            break;

          /* Get the number of bytes that have been written to the TX
           * buffer.
           */

          case FIONWRITE:
            {
              int count;
              irqstate_t flags = enter_critical_section();

              /* Determine the number of bytes waiting in the TX buffer */

              if (dev->xmit.tail <= dev->xmit.head)
                {
                  count = dev->xmit.head - dev->xmit.tail;
                }
              else
                {
                  count = dev->xmit.size - (dev->xmit.tail - dev->xmit.head);
                }

              leave_critical_section(flags);

              *(FAR int *)((uintptr_t)arg) = count;
              ret = 0;
            }
            break;

          /* Get the number of free bytes in the TX buffer */

          case FIONSPACE:
            {
              int count;
              irqstate_t flags = enter_critical_section();

              /* Determine the number of bytes free in the TX buffer */

              if (dev->xmit.head < dev->xmit.tail)
                {
                  count = dev->xmit.tail - dev->xmit.head - 1;
                }
              else
                {
                  count = dev->xmit.size -
                         (dev->xmit.head - dev->xmit.tail) - 1;
                }

              leave_critical_section(flags);

              *(FAR int *)((uintptr_t)arg) = count;
              ret = 0;
            }
            break;

          case TCFLSH:
            {
              /* Empty the tx/rx buffers */

              irqstate_t flags = enter_critical_section();

              if (arg == TCIFLUSH || arg == TCIOFLUSH)
                {
                  dev->recv.tail = dev->recv.head;

#ifdef CONFIG_SERIAL_IFLOWCONTROL
                  /* De-activate RX flow control. */

                  uart_rxflowcontrol(dev, 0, false);
#endif
                }

              if (arg == TCOFLUSH || arg == TCIOFLUSH)
                {
                  dev->xmit.tail = dev->xmit.head;

                  /* Inform any waiters there there is space available. */

                  uart_datasent(dev);
                }

              leave_critical_section(flags);
              ret = 0;
            }
            break;

          case TCDRN:
            {
              ret = uart_tcdrain(dev, true, 10 * TICK_PER_SEC);
            }
            break;

          case TCSBRK:
            {
              /* Non-standard Break specifies duration in milliseconds */

              ret = uart_tcsendbreak(dev, filep, arg);
            }
            break;

          case TCSBRKP:
            {
              /* POSIX Break specifies duration in units of 100ms */

              ret = uart_tcsendbreak(dev, filep, arg * 100);
            }
            break;

#if defined(CONFIG_TTY_SIGINT) || defined(CONFIG_TTY_SIGTSTP)
          /* Make the controlling terminal of the calling process */

          case TIOCSCTTY:
            {
              /* Save the PID of the recipient of the SIGINT signal. */

              if ((int)arg < 0 || dev->pid >= 0)
                {
                  ret = -EINVAL;
                }
              else
                {
                  dev->pid = (pid_t)arg;
                  ret = 0;
                }
            }
            break;

          case TIOCNOTTY:
            {
              dev->pid = INVALID_PROCESS_ID;
              ret = 0;
            }
            break;
#endif
        }
    }

  /* Append any higher level TTY flags */

  if (ret == OK || ret == -ENOTTY)
    {
      switch (cmd)
        {
          case TCGETS:
            {
              if (!termiosp)
                {
                  ret = -EINVAL;
                  break;
                }

              /* And update with flags from this layer */

              termiosp->c_iflag = dev->tc_iflag;
              termiosp->c_oflag = dev->tc_oflag;
              termiosp->c_lflag = dev->tc_lflag;
#ifdef CONFIG_SERIAL_TERMIOS
              termiosp->c_cc[VTIME] = dev->timeout;
              termiosp->c_cc[VMIN] = dev->minread;
#endif

              ret = 0;
            }
            break;

          case TCSETS:
            {
              if (!termiosp)
                {
                  ret = -EINVAL;
                  break;
                }

              /* Update the flags we keep at this layer */

              dev->tc_iflag = termiosp->c_iflag;
              dev->tc_oflag = termiosp->c_oflag;
              dev->tc_lflag = termiosp->c_lflag;
#ifdef CONFIG_SERIAL_TERMIOS
              dev->timeout = termiosp->c_cc[VTIME];
              dev->minread = termiosp->c_cc[VMIN];
              dev->minrecv = dev->minread;
#endif
              ret = 0;
            }
            break;
        }
    }

  return ret;
}

/****************************************************************************
 * Name: uart_poll
 ****************************************************************************/

static int uart_poll(FAR struct file *filep,
                     FAR struct pollfd *fds, bool setup)
{
  FAR struct inode *inode = filep->f_inode;
  FAR uart_dev_t   *dev   = inode->i_private;
  pollevent_t       eventset;
  irqstate_t        flags;
  int               ndx;
  int               ret = OK;
  int               i;

  /* Some sanity checking */

#ifdef CONFIG_DEBUG_FEATURES
  if (dev == NULL || fds == NULL)
    {
      return -ENODEV;
    }
#endif

  flags = enter_critical_section();

  /* Are we setting up the poll?  Or tearing it down? */

  if (setup)
    {
      /* This is a request to set up the poll.  Find an available
       * slot for the poll structure reference
       */

      for (i = 0; i < CONFIG_SERIAL_NPOLLWAITERS; i++)
        {
          /* Find an available slot */

          if (!dev->fds[i])
            {
              /* Bind the poll structure and this slot */

              dev->fds[i] = fds;
              fds->priv   = &dev->fds[i];
              break;
            }
        }

      if (i >= CONFIG_SERIAL_NPOLLWAITERS)
        {
          fds->priv = NULL;
          ret       = -EBUSY;
          goto errout;
        }

      leave_critical_section(flags);

      /* Should we immediately notify on any of the requested events?
       * First, check if the xmit buffer is full.
       *
       * Get exclusive access to the xmit buffer indices.
       * NOTE: that we do not let this wait be interrupted by a signal
       * (we probably should, but that would be a little awkward).
       */

      eventset = 0;
      nxmutex_lock(&dev->xmit.lock);

      ndx = dev->xmit.head + 1;
      if (ndx >= dev->xmit.size)
        {
          ndx = 0;
        }

      if (ndx != dev->xmit.tail)
        {
          eventset |= POLLOUT;
        }

      nxmutex_unlock(&dev->xmit.lock);

      /* Check if the receive buffer is empty.
       *
       * Get exclusive access to the recv buffer indices.
       * NOTE: that we do not let this wait be interrupted by a signal
       * (we probably should, but that would be a little awkward).
       */

      nxmutex_lock(&dev->recv.lock);
      if (dev->recv.head != dev->recv.tail)
        {
          eventset |= POLLIN;
        }

      nxmutex_unlock(&dev->recv.lock);

#ifdef CONFIG_SERIAL_REMOVABLE
      /* Check if a removable device has been disconnected. */

      if (dev->disconnected)
        {
           eventset |= (POLLERR | POLLHUP);
        }
#endif

      uart_poll_notify(dev, i, i + 1, eventset);
    }
  else if (fds->priv != NULL)
    {
      /* This is a request to tear down the poll. */

      FAR struct pollfd **slot = (FAR struct pollfd **)fds->priv;

#ifdef CONFIG_DEBUG_FEATURES
      if (!slot)
        {
          ret = -EIO;
          goto errout;
        }
#endif

      /* Remove all memory of the poll setup */

      *slot     = NULL;
      fds->priv = NULL;

      leave_critical_section(flags);
    }

  return ret;

errout:
  leave_critical_section(flags);
  return ret;
}

/****************************************************************************
 * Name: uart_unlink
 ****************************************************************************/

#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
static int uart_unlink(FAR struct inode *inode)
{
  FAR uart_dev_t *dev;
  int ret;

  DEBUGASSERT(inode->i_private != NULL);

  dev = inode->i_private;
  ret = nxmutex_lock(&dev->closelock);
  if (ret < 0)
    {
      /* A signal received while waiting for the last close operation. */

      return ret;
    }

  if (dev->open_count <= 0)
    {
      nxmutex_unlock(&dev->closelock);
      nxmutex_destroy(&dev->xmit.lock);
      nxmutex_destroy(&dev->recv.lock);
      nxmutex_destroy(&dev->closelock);
      nxsem_destroy(&dev->xmitsem);
      nxsem_destroy(&dev->recvsem);
      uart_release(dev);
      return OK;
    }

  dev->unlinked = true;
  nxmutex_unlock(&dev->closelock);
  return OK;
}
#endif

/****************************************************************************
 * Name: uart_nxsched_foreach_cb
 ****************************************************************************/

#ifdef CONFIG_TTY_LAUNCH
static void uart_launch_foreach(FAR struct tcb_s *tcb, FAR void *arg)
{
#ifdef CONFIG_TTY_LAUNCH_ENTRY
  if (!strcmp(get_task_name(tcb), CONFIG_TTY_LAUNCH_ENTRYNAME))
#else
  if (!strcmp(get_task_name(tcb), CONFIG_TTY_LAUNCH_FILEPATH))
#endif
    {
      *(FAR int *)arg = 1;
    }
}

static void uart_launch_worker(void *arg)
{
#ifdef CONFIG_TTY_LAUNCH_ARGS
  FAR char *const argv[] =
  {
    CONFIG_TTY_LAUNCH_ARGS,
    NULL,
  };
#else
  FAR char *const *argv = NULL;
#endif
  int found = 0;

  nxsched_foreach(uart_launch_foreach, &found);
  if (!found)
    {
      posix_spawnattr_t attr;

      posix_spawnattr_init(&attr);
      attr.priority  = CONFIG_TTY_LAUNCH_PRIORITY;
      attr.stacksize = CONFIG_TTY_LAUNCH_STACKSIZE;

#ifdef CONFIG_TTY_LAUNCH_ENTRY
      task_spawn(CONFIG_TTY_LAUNCH_ENTRYNAME,
                 CONFIG_TTY_LAUNCH_ENTRYPOINT,
                 NULL, &attr, argv, NULL);
#else
      exec_spawn(CONFIG_TTY_LAUNCH_FILEPATH,
                 argv, NULL, NULL, 0, NULL, &attr);
#endif
      posix_spawnattr_destroy(&attr);
    }
}

static void uart_launch(void)
{
  work_queue(HPWORK, &g_serial_work, uart_launch_worker, NULL, 0);
}
#endif

static void uart_wakeup(FAR sem_t *sem)
{
  int sval;

  if (nxsem_get_value(sem, &sval) != OK)
    {
      return;
    }

  /* Yes... wake up all waiting threads */

  while (sval++ < 1)
    {
      nxsem_post(sem);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: uart_register
 *
 * Description:
 *   Register serial console and serial ports.
 *
 ****************************************************************************/

int uart_register(FAR const char *path, FAR uart_dev_t *dev)
{
#if defined(CONFIG_TTY_SIGINT) || defined(CONFIG_TTY_SIGTSTP)
  /* Initialize  of the task that will receive SIGINT signals. */

  dev->pid = INVALID_PROCESS_ID;
#endif

#ifdef CONFIG_TTY_FORCE_PANIC
  dev->panic_count = 0;
#endif

  /* If this UART is a serial console */

  if (dev->isconsole)
    {
      /* Enable signals and echo by default */

      dev->tc_lflag |= ISIG | ECHO | ICANON;

      /* Enable \n -> \r\n translation for the console */

      dev->tc_oflag = OPOST | ONLCR;

      /* Convert CR to LF by default for console */

      dev->tc_iflag |= ICRNL;

      /* Clear escape counter */

      dev->escape = 0;
    }

  /* Initialize mutex & semaphores */

  nxmutex_init(&dev->xmit.lock);
  nxmutex_init(&dev->recv.lock);
  nxmutex_init(&dev->closelock);
  nxsem_init(&dev->xmitsem, 0, 0);
  nxsem_init(&dev->recvsem, 0, 0);

#ifdef CONFIG_SERIAL_TERMIOS
  dev->timeout = 0;
  dev->minread = 1;
#endif

  /* Register the serial driver */

#ifdef CONFIG_SERIAL_GDBSTUB
  if (uart_gdbstub_register(dev, path) == 0)
    {
      /* No need register the device if it is used by gdbstub */

      return 0;
    }
#endif

  sinfo("Registering %s\n", path);
  return register_driver(path, &g_serialops, 0666, dev);
}

/****************************************************************************
 * Name: uart_datareceived
 *
 * Description:
 *   This function is called from uart_recvchars when new serial data is
 *   place in the driver's circular buffer.  This function will wake-up any
 *   stalled read() operations that are waiting for incoming data.
 *
 ****************************************************************************/

void uart_datareceived(FAR uart_dev_t *dev)
{
  /* Notify all poll/select waiters that they can read from the recv buffer */

  uart_poll_notify(dev, 0, CONFIG_SERIAL_NPOLLWAITERS, POLLIN);

  /* Is there a thread waiting for read data?  */

  uart_wakeup(&dev->recvsem);

#if defined(CONFIG_PM) && defined(CONFIG_SERIAL_CONSOLE)
  /* Call pm_activity when characters are received on the console device */

  if (dev->isconsole)
    {
#  if CONFIG_SERIAL_PM_ACTIVITY_PRIORITY > 0
      pm_activity(CONFIG_SERIAL_PM_ACTIVITY_DOMAIN,
                  CONFIG_SERIAL_PM_ACTIVITY_PRIORITY);
#  endif
    }
#endif
}

/****************************************************************************
 * Name: uart_datasent
 *
 * Description:
 *   This function is called from uart_xmitchars after serial data has been
 *   sent, freeing up some space in the driver's circular buffer. This
 *   function will wake-up any stalled write() operations that was waiting
 *   for space to buffer outgoing data.
 *
 ****************************************************************************/

void uart_datasent(FAR uart_dev_t *dev)
{
  /* Notify all poll/select waiters that they can write to xmit buffer */

  uart_poll_notify(dev, 0, CONFIG_SERIAL_NPOLLWAITERS, POLLOUT);

  /* Is there a thread waiting for space in xmit.buffer?  */

  uart_wakeup(&dev->xmitsem);
}

/****************************************************************************
 * Name: uart_connected
 *
 * Description:
 *   Serial devices (like USB serial) can be removed.
 *   In that case, the "upper half" serial driver must be informed that there
 *   is no longer a valid serial channel associated with the driver.
 *
 *   In this case, the driver will terminate all pending transfers wint
 *   ENOTCONN and will refuse all further transactions while the "lower half"
 *   is disconnected.
 *   The driver will continue to be registered, but will be in an unusable
 *   state.
 *
 *   Conversely, the "upper half" serial driver needs to know when the serial
 *   device is reconnected so that it can resume normal operations.
 *
 * Assumptions/Limitations:
 *   This function may be called from an interrupt handler.
 *
 ****************************************************************************/

#ifdef CONFIG_SERIAL_REMOVABLE
void uart_connected(FAR uart_dev_t *dev, bool connected)
{
  irqstate_t flags;

  /* Is the device disconnected?  Interrupts are disabled because this
   * function may be called from interrupt handling logic.
   */

  flags = enter_critical_section();
  sched_lock();
  dev->disconnected = !connected;
  if (!connected)
    {
      /* Notify all poll/select waiters that a hangup occurred */

      poll_notify(dev->fds, CONFIG_SERIAL_NPOLLWAITERS, POLLERR | POLLHUP);

      /* Yes.. wake up all waiting threads.  Each thread should detect the
       * disconnection and return the ENOTCONN error.
       */

      /* Is there a thread waiting for space in xmit.buffer?  */

      uart_wakeup(&dev->xmitsem);

      /* Is there a thread waiting for read data?  */

      uart_wakeup(&dev->recvsem);
    }

  sched_unlock();
  leave_critical_section(flags);
}
#endif

/****************************************************************************
 * Name: uart_reset_sem
 *
 * Description:
 *   This function is called when need reset uart semaphore, this may used in
 *   kill one process, but this process was reading/writing with the
 *   semaphore.
 *
 ****************************************************************************/

void uart_reset_sem(FAR uart_dev_t *dev)
{
  nxsem_reset(&dev->xmitsem,  0);
  nxsem_reset(&dev->recvsem,  0);
  nxmutex_reset(&dev->xmit.lock);
  nxmutex_reset(&dev->recv.lock);
}

/****************************************************************************
 * Name: uart_check_special
 *
 * Description:
 *   Check if the SIGINT or SIGTSTP character is in the contiguous Rx DMA
 *   buffer region.  The first signal associated with the first such
 *   character is returned.
 *
 *   If there multiple such characters in the buffer, only the signal
 *   associated with the first is returned (this a bug!)
 *
 * Returned Value:
 *   0 if a signal-related character does not appear in the.  Otherwise,
 *   SIGKILL or SIGTSTP may be returned to indicate the appropriate signal
 *   action.
 *
 ****************************************************************************/

#if defined(CONFIG_TTY_SIGINT) || defined(CONFIG_TTY_SIGTSTP) || \
    defined(CONFIG_TTY_FORCE_PANIC) || defined(CONFIG_TTY_LAUNCH)
int uart_check_special(FAR uart_dev_t *dev, FAR const char *buf, size_t size)
{
  size_t i;

  if ((dev->tc_lflag & ISIG) == 0)
    {
      return 0;
    }

  for (i = 0; i < size; i++)
    {
#ifdef CONFIG_TTY_FORCE_PANIC
      if (buf[i] == CONFIG_TTY_FORCE_PANIC_CHAR)
        {
          if (++dev->panic_count >= CONFIG_TTY_FORCE_PANIC_REPEAT_COUNT)
            {
              PANIC_WITH_REGS("Force panic by user.", NULL);
            }

          return 0;
        }
      else
        {
          dev->panic_count = 0;
        }
#endif

#ifdef CONFIG_TTY_LAUNCH
      if (buf[i] == CONFIG_TTY_LAUNCH_CHAR)
        {
          uart_launch();
          return 0;
        }
#endif

#ifdef CONFIG_TTY_SIGINT
      if (dev->pid > 0 && buf[i] == CONFIG_TTY_SIGINT_CHAR)
        {
          return SIGINT;
        }
#endif

#ifdef CONFIG_TTY_SIGTSTP
      if (dev->pid > 0 && buf[i] == CONFIG_TTY_SIGTSTP_CHAR)
        {
          return SIGTSTP;
        }
#endif
    }

  return 0;
}
#endif
