/*
 * Copyright (C) 2017~2017 by CSSlayer
 * wengxt@gmail.com
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; see the file COPYING. If not,
 * see <http://www.gnu.org/licenses/>.
 */
#ifndef _FCITX_LIBIME_LATTICE_P_H_
#define _FCITX_LIBIME_LATTICE_P_H_

#include "lattice.h"
#include "segmentgraph.h"
#include <boost/ptr_container/ptr_vector.hpp>
#include <map>
#include <unordered_map>

namespace libime {

typedef std::unordered_map<const SegmentGraphNode *,
                           boost::ptr_vector<LatticeNode>>
    LatticeMap;

class LatticePrivate {
public:
    LatticeMap lattice;

    std::vector<SentenceResult> nbests;
};
}

#endif // _FCITX_LIBIME_LATTICE_P_H_
