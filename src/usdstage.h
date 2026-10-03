/*
    usdstage.h: a USD file composed into a stage (usd.h reads one layer).

    The layer stack (subLayers) and, for every prim, its composition arcs:
    inherits, the selected variants, references, payloads (always loaded)
    and specializes, in strength order (LIVRPS: the local layer stack
    first; among arcs of the same kind, those authored on the prim itself
    before those of its ancestors). The result is a Layer named after the
    file, whose prims are the composed prims: specifier, type, metadata and
    properties resolved strongest first, relationship targets mapped into the
    stage namespace. Property values stay in their layers and are read on
    demand (Property::origin). A file without composition arcs is returned
    as read.

    Not composed: layer offsets and scales (time), relocates, variant
    fallbacks, value clips, payloads left unloaded; a package (.usdz) cannot
    open the files it contains, only its root layer.
*/

#pragma once

#include "usd.h"

namespace usd {

/// The composed stage of a USD file (see above); throws if the file
/// cannot be read, warns and skips an arc whose target cannot be found
std::shared_ptr<const Layer> open_stage(const std::string &filename);

} // namespace usd
