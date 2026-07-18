// EncoderFactory.h - the one place a RipFormat becomes a concrete IEncoder.
//
// makeEncoder was file-static in CDRipper.cpp (the rip's only caller). The
// convert-core slice adds a second caller (ConvertJob), so the definition is
// widened to external linkage and declared here. This is a LINKAGE change only:
// the switch body and the encoder ctor arguments are unchanged, so rip output
// (pinned by rip_encoder_seam_test) is byte-identical.
#pragma once

#include "IEncoder.h"
#include "RipFormats.h"   // RipFormat + RipOptions

#include <memory>

std::unique_ptr<IEncoder> makeEncoder(RipFormat f, const RipOptions& opt);
