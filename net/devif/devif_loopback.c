/****************************************************************************
 * net/devif/devif_loopback.c
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

#include <string.h>
#include <debug.h>

#include <nuttx/net/ip.h>
#include <nuttx/net/pkt.h>
#include <nuttx/net/netdev.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: devif_is_loopback
 *
 * Description:
 *   The function checks the destination address of the packet to see
 *   whether the target of packet is ourself.
 *
 * Returned Value:
 *   true is returned if the packet need loop back to ourself, otherwise
 *   false is returned.
 *
 ****************************************************************************/

bool devif_is_loopback(FAR struct net_driver_s *dev)
{
  if (dev->d_len > 0)
    {
#ifdef CONFIG_NET_IPv4
      if ((IPv4BUF->vhl & IP_VERSION_MASK) == IPv4_VERSION &&
           net_ipv4addr_hdrcmp(IPv4BUF->destipaddr, &dev->d_ipaddr))
        {
          return true;
        }
#endif

#ifdef CONFIG_NET_IPv6
      if ((IPv6BUF->vtc & IP_VERSION_MASK) == IPv6_VERSION &&
          NETDEV_IS_MY_V6ADDR(dev, IPv6BUF->destipaddr))
        {
          return true;
        }
#endif
    }

  return false;
}

/****************************************************************************
 * Name: devif_loopback
 *
 * Description:
 *   This function should be called before sending out a packet. The function
 *   checks the destination address of the packet to see whether the target
 *   of packet is ourself and then consume the packet directly by calling
 *   input process functions.
 *
 * Returned Value:
 *   Zero is returned if the packet don't loop back to ourself, otherwise
 *   a non-zero value is returned.
 *
 ****************************************************************************/

int devif_loopback(FAR struct net_driver_s *dev)
{
  if (!devif_is_loopback(dev))
    {
      return 0;
    }

  /* Loop while if there is data "sent" to ourself.
   * Sending, of course, just means relaying back through the network.
   */

  do
    {
       NETDEV_TXPACKETS(dev);
       NETDEV_RXPACKETS(dev);

#ifdef CONFIG_NET_PKT
      /* When packet sockets are enabled, feed the frame into the tap */

       pkt_input(dev);
#endif

      /* We only accept IP packets of the configured type */

#ifdef CONFIG_NET_IPv4
      if ((IPv4BUF->vhl & IP_VERSION_MASK) == IPv4_VERSION)
        {
          ninfo("IPv4 frame\n");

          NETDEV_RXIPV4(dev);
          ipv4_input(dev);
        }
      else
#endif
#ifdef CONFIG_NET_IPv6
      if ((IPv6BUF->vtc & IP_VERSION_MASK) == IPv6_VERSION)
        {
          ninfo("IPv6 frame\n");

          NETDEV_RXIPV6(dev);
          ipv6_input(dev);
        }
      else
#endif
        {
          nwarn("WARNING: Unrecognized IP version\n");
          NETDEV_RXDROPPED(dev);
          dev->d_len = 0;
        }

      NETDEV_TXDONE(dev);
    }
  while (dev->d_len > 0);

  return 1;
}
