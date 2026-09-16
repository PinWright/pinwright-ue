// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class IIrGrammar;

// BPIR opcode + syntax-keyword table for FIrTokenizer. Callers tokenize a line
// with FIrTokenizer::Tokenize(Line, GetBpirGrammar()).
const IIrGrammar& GetBpirGrammar();
