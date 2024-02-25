/**
Copyright (c) 1994, 2009, Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*****************************************************************************/

/** @file ut/ut0byte.c
Byte utilities

Created 5/11/1994 Heikki Tuuri
********************************************************************/

#include "ut0byte.h"

#ifdef UNIV_NONINL
#include "ut0byte.ic"
#endif

/** Zero value for a dulint */
const dulint ut_dulint_zero = {0, 0};

/** Maximum value for a dulint */
const dulint ut_dulint_max = {0xFFFFFFFFUL, 0xFFFFFFFFUL};

#ifdef notdefined /* unused code */
#include "ut0sort.h"

/** Sort function for dulint arrays. */

void ut_dulint_sort(
    dulint *arr,     /*!< in/out: array to be sorted */
    dulint *aux_arr, /*!< in/out: auxiliary array (same size as arr) */
    ulint low,       /*!< in: low bound of sort interval, inclusive */
    ulint high)      /*!< in: high bound of sort interval, noninclusive */
{
  UT_SORT_FUNCTION_BODY(ut_dulint_sort, arr, aux_arr, low, high, ut_dulint_cmp);
}
#endif /* notdefined */
