#pragma once

#include <string>
#include "ModelTypes.h"

// Parses an IFC file in-process with web-ifc: tessellates every element (one submesh per colour of
// each element) and builds the IfcScene (spatial tree, attributes, type
// info, property/quantity sets and materials) straight from the STEP data.
// Throws std::runtime_error on failure.
Mesh loadIfcDirect(const std::string& ifcPath);
