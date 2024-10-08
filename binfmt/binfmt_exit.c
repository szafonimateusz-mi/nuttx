/****************************************************************************
 * binfmt/binfmt_exit.c
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

#include <sys/types.h>
#include <assert.h>
#include <debug.h>

#include <nuttx/kmalloc.h>
#include <nuttx/binfmt/binfmt.h>

#include "binfmt.h"

#ifdef CONFIG_BINFMT_LOADABLE

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: binfmt_exit
 *
 * Description:
 *   This function may be called when a tasked loaded into RAM exits.
 *   This function will unload the module when the task exits and reclaim
 *   all resources used by the module.
 *
 * Input Parameters:
 *   bin - This structure must have been allocated with kmm_malloc() and must
 *         persist until the task unloads
 *
 * Returned Value:
 *   This is a NuttX internal function so it follows the convention that
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

int binfmt_exit(FAR struct binary_s *bin)
{
  int ret;

  DEBUGASSERT(bin != NULL);

  /* Unload the module */

  ret = unload_module(bin);
  if (ret < 0)
    {
      berr("ERROR: unload_module() failed: %d\n", ret);
    }

  /* Free the load structure */

  kmm_free(bin);
  return ret;
}

#endif /* CONFIG_BINFMT_LOADABLE */
