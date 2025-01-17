/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2022 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>   // For std::memset
#include <fstream>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <streambuf>
#include <vector>

#include "bitboard.h"
#include "evaluate.h"
#include "material.h"
#include "misc.h"
#include "pawns.h"
#include "thread.h"
#include "timeman.h"
#include "uci.h"
#include "incbin/incbin.h"


// Macro to embed the default efficiently updatable neural network (NNUE) file
// data in the engine binary (using incbin.h, by Dale Weiler).
// This macro invocation will declare the following three variables
//     const unsigned char        gEmbeddedNNUEData[];  // a pointer to the embedded data
//     const unsigned char *const gEmbeddedNNUEEnd;     // a marker to the end
//     const unsigned int         gEmbeddedNNUESize;    // the size of the embedded file
// Note that this does not work in Microsoft Visual Studio.
#ifdef USE_NNUE
#if !defined(_MSC_VER) && !defined(NNUE_EMBEDDING_OFF)
  INCBIN(EmbeddedNNUE, EvalFileDefaultName);
#else
  const unsigned char        gEmbeddedNNUEData[1] = {0x0};
  const unsigned char *const gEmbeddedNNUEEnd = &gEmbeddedNNUEData[1];
  const unsigned int         gEmbeddedNNUESize = 1;
#endif
#endif


using namespace std;

namespace Stockfish {

#ifdef USE_NNUE
namespace Eval {

  bool useNNUE;
  string currentEvalFileName = "None";

  /// NNUE::init() tries to load a NNUE network at startup time, or when the engine
  /// receives a UCI command "setoption name EvalFile value nn-[a-z0-9]{12}.nnue"
  /// The name of the NNUE network is always retrieved from the EvalFile option.
  /// We search the given network in three locations: internally (the default
  /// network may be embedded in the binary), in the active working directory and
  /// in the engine directory. Distro packagers may define the DEFAULT_NNUE_DIRECTORY
  /// variable to have the engine search in a special directory in their distro.

  void NNUE::init() {

    useNNUE = Options["Use NNUE"];
    if (!useNNUE)
        return;

    string eval_file = string(Options["EvalFile"]);
    if (eval_file.empty())
        eval_file = EvalFileDefaultName;

    #if defined(DEFAULT_NNUE_DIRECTORY)
    #define stringify2(x) #x
    #define stringify(x) stringify2(x)
    vector<string> dirs = { "<internal>" , "" , CommandLine::binaryDirectory , stringify(DEFAULT_NNUE_DIRECTORY) };
    #else
    vector<string> dirs = { "<internal>" , "" , CommandLine::binaryDirectory };
    #endif

    for (string directory : dirs)
        if (currentEvalFileName != eval_file)
        {
            if (directory != "<internal>")
            {
                ifstream stream(directory + eval_file, ios::binary);
                if (load_eval(eval_file, stream))
                    currentEvalFileName = eval_file;
            }

            if (directory == "<internal>" && eval_file == EvalFileDefaultName)
            {
                // C++ way to prepare a buffer for a memory stream
                class MemoryBuffer : public basic_streambuf<char> {
                    public: MemoryBuffer(char* p, size_t n) { setg(p, p, p + n); setp(p, p + n); }
                };

                MemoryBuffer buffer(const_cast<char*>(reinterpret_cast<const char*>(gEmbeddedNNUEData)),
                                    size_t(gEmbeddedNNUESize));

                istream stream(&buffer);
                if (load_eval(eval_file, stream))
                    currentEvalFileName = eval_file;
            }
        }
  }

  /// NNUE::verify() verifies that the last net used was loaded successfully
  void NNUE::verify() {

    string eval_file = string(Options["EvalFile"]);
    if (eval_file.empty())
        eval_file = EvalFileDefaultName;

    if (useNNUE && currentEvalFileName != eval_file)
    {

        string msg1 = "If the UCI option \"Use NNUE\" is set to true, network evaluation parameters compatible with the engine must be available.";
        string msg2 = "The option is set to true, but the network file " + eval_file + " was not loaded successfully.";
        string msg3 = "The UCI option EvalFile might need to specify the full path, including the directory name, to the network file.";
        string msg4 = "The default net can be downloaded from: https://tests.stockfishchess.org/api/nn/" + std::string(EvalFileDefaultName);
        string msg5 = "The engine will be terminated now.";

        sync_cout << "info string ERROR: " << msg1 << sync_endl;
        sync_cout << "info string ERROR: " << msg2 << sync_endl;
        sync_cout << "info string ERROR: " << msg3 << sync_endl;
        sync_cout << "info string ERROR: " << msg4 << sync_endl;
        sync_cout << "info string ERROR: " << msg5 << sync_endl;

        exit(EXIT_FAILURE);
    }

    if (useNNUE)
        sync_cout << "info string NNUE evaluation using " << eval_file << " enabled" << sync_endl;
    else
        sync_cout << "info string classical evaluation enabled" << sync_endl;
  }
}
#endif

namespace Trace {

  enum Tracing { NO_TRACE, TRACE };

  enum Term { // The first 8 entries are reserved for PieceType
    MATERIAL = 8, IMBALANCE, MOBILITY, THREAT, PASSED, SPACE, WINNABLE, VARIANT, TOTAL, TERM_NB
  };

  Score scores[TERM_NB][COLOR_NB];

  double to_cp(Value v) { return double(v) / PawnValueEg; }

  void add(int idx, Color c, Score s) {
    scores[idx][c] = s;
  }

  void add(int idx, Score w, Score b = SCORE_ZERO) {
    scores[idx][WHITE] = w;
    scores[idx][BLACK] = b;
  }

  std::ostream& operator<<(std::ostream& os, Score s) {
    os << std::setw(5) << to_cp(mg_value(s)) << " "
       << std::setw(5) << to_cp(eg_value(s));
    return os;
  }

  std::ostream& operator<<(std::ostream& os, Term t) {

    if (t == MATERIAL || t == IMBALANCE || t == WINNABLE || t == TOTAL)
        os << " ----  ----"    << " | " << " ----  ----";
    else
        os << scores[t][WHITE] << " | " << scores[t][BLACK];

    os << " | " << scores[t][WHITE] - scores[t][BLACK] << " |\n";
    return os;
  }
}

using namespace Trace;

namespace {

  // Threshold for lazy and space evaluation
  constexpr Value LazyThreshold1[VARIANT_NB] = {
    Value(3631),
#ifdef ANTI
    2 * MidgameLimit,
#endif
#ifdef ATOMIC
    Value(3094),
#endif
#ifdef CRAZYHOUSE
    2 * MidgameLimit,
#endif
#ifdef EXTINCTION
    Value(3631),
#endif
#ifdef GRID
    Value(3631),
#endif
#ifdef HORDE
    2 * MidgameLimit,
#endif
#ifdef KOTH
    Value(3631),
#endif
#ifdef LOSERS
    2 * MidgameLimit,
#endif
#ifdef RACE
    2 * MidgameLimit,
#endif
#ifdef THREECHECK
    Value(3058),
#endif
#ifdef TWOKINGS
    Value(3631),
#endif
  };
  constexpr Value LazyThreshold2    =  Value(2084);
  constexpr Value SpaceThreshold[VARIANT_NB] = {
    Value(11551),
#ifdef ANTI
    Value(11551),
#endif
#ifdef ATOMIC
    Value(11551),
#endif
#ifdef CRAZYHOUSE
    Value(11551),
#endif
#ifdef EXTINCTION
    Value(11551),
#endif
#ifdef GRID
    2 * MidgameLimit,
#endif
#ifdef HORDE
    Value(11551),
#endif
#ifdef KOTH
    VALUE_ZERO,
#endif
#ifdef LOSERS
    Value(11551),
#endif
#ifdef RACE
    Value(11551),
#endif
#ifdef THREECHECK
    Value(11551),
#endif
#ifdef TWOKINGS
    Value(11551),
#endif
  };

  // KingAttackWeights[PieceType] contains king attack weights by piece type
  constexpr int KingAttackWeights[VARIANT_NB][PIECE_TYPE_NB] = {
    { 0, 0, 81, 52, 44, 10 },
#ifdef ANTI
    {},
#endif
#ifdef ATOMIC
    { 0, 0, 76, 64, 46, 11 },
#endif
#ifdef CRAZYHOUSE
    { 0, 0, 112, 87, 63, 2 },
#endif
#ifdef EXTINCTION
    {},
#endif
#ifdef GRID
    { 0, 0, 89, 62, 47, 11 },
#endif
#ifdef HORDE
    { 0, 0, 77, 55, 44, 10 },
#endif
#ifdef KOTH
    { 0, 0, 76, 48, 44, 10 },
#endif
#ifdef LOSERS
    { 0, 0, 77, 55, 44, 10 },
#endif
#ifdef RACE
    {},
#endif
#ifdef THREECHECK
    { 0, 0, 118, 66, 62, 35 },
#endif
#ifdef TWOKINGS
    { 0, 0, 77, 55, 44, 10 },
#endif
  };

  // Per-variant king danger malus factors
  constexpr int KingDangerParams[VARIANT_NB][11] = {
    {   183,  148,   98,   69,    3, -873, -100,   -6,   -4,   37,    0 },
#ifdef ANTI
    {},
#endif
#ifdef ATOMIC
    {   166,  146,   98,  274,    3, -654, -100,  -12,   -4,   37,   29 },
#endif
#ifdef CRAZYHOUSE
    {   463,  129,   99,  121,    3, -631,  -99,   -6,   -4,   37,  315 },
#endif
#ifdef EXTINCTION
    {},
#endif
#ifdef GRID
    {   211,  158,   98,  119,    3, -722, -100,   -9,   -4,   37,    0 },
#endif
#ifdef HORDE
    {   235,  134,   98,  101,    3, -717, -100,  -11,   -4,   37,    0 },
#endif
#ifdef KOTH
    {   229,  131,   98,   85,    3, -658, -100,   -9,   -4,   37,    0 },
#endif
#ifdef LOSERS
    {   235,  134,   98,  101,    3, -717, -100, -357,   -4,   37,    0 },
#endif
#ifdef RACE
    {     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0 },
#endif
#ifdef THREECHECK
    {   203,  149,  101,   72,    3, -787,  -91,   -6,   -4,   38,  192 },
#endif
#ifdef TWOKINGS
    {   155,  136,   98,   92,    3, -967, -100,   -8,   -4,   37,    0 },
#endif
  };

  // SafeCheck[PieceType][single/multiple] contains safe check bonus by piece type,
  // higher if multiple safe checks are possible for that piece type.
  constexpr int SafeCheck[][2] = {
      {}, {450, 900}, {803, 1292}, {639, 974}, {1087, 1878}, {759, 1132}
  };

#define S(mg, eg) make_score(mg, eg)

  // MobilityBonus[PieceType-2][attacked] contains bonuses for middle and end game,
  // indexed by piece type and number of attacked squares in the mobility area.
  constexpr Score MobilityBonus[VARIANT_NB][4][32] = {
    {
    { S(-62,-79), S(-53,-57), S(-12,-31), S( -3,-17), S(  3,  7), S( 12, 13), // Knight
      S( 21, 16), S( 28, 21), S( 37, 26) },
    { S(-47,-59), S(-20,-25), S( 14, -8), S( 29, 12), S( 39, 21), S( 53, 40), // Bishop
      S( 53, 56), S( 60, 58), S( 62, 65), S( 69, 72), S( 78, 78), S( 83, 87),
      S( 91, 88), S( 96, 98) },
    { S(-60,-82), S(-24,-15), S(  0, 17) ,S(  3, 43), S(  4, 72), S( 14,100), // Rook
      S( 20,102), S( 30,122), S( 41,133), S(41 ,139), S( 41,153), S( 45,160),
      S( 57,165), S( 58,170), S( 67,175) },
    { S(-29,-49), S(-16,-29), S( -8, -8), S( -8, 17), S( 18, 39), S( 25, 54), // Queen
      S( 23, 59), S( 37, 73), S( 41, 76), S( 54, 95), S( 65, 95) ,S( 68,101),
      S( 69,124), S( 70,128), S( 70,132), S( 70,133) ,S( 71,136), S( 72,140),
      S( 74,147), S( 76,149), S( 90,153), S(104,169), S(105,171), S(106,171),
      S(112,178), S(114,185), S(114,187), S(119,221) }
    },
#ifdef ANTI
    {
      { S(-150,-152), S(-112,-108), S(-18,-52), S( -4,-20), S( 12, 10), S( 30, 22), // Knights
        S(  44,  52), S(  60,  56), S( 72, 58) },
      { S(-96,-116), S(-42,-38), S( 32, -4), S( 52, 24), S( 74, 44), S(102, 84), // Bishops
        S(108, 108), S(126,116), S(130,126), S(142,140), S(158,148), S(162,172),
        S(184, 180), S(194,188) },
      { S(-112,-156), S(-50,-36), S(-22, 52), S(-10,110), S( -8,140), S( -2,162), // Rooks
        S(  16, 218), S( 28,240), S( 42,256), S( 46,286), S( 62,308), S( 64,320),
        S(  86, 330), S( 98,336), S(118,338) },
      { S(-80,-70), S(-50,-24), S(  4, 14), S(  8, 38), S( 28, 74), S( 48,110), // Queens
        S( 50,124), S( 80,152), S( 86,158), S( 94,174), S(108,188), S(112,204),
        S(120,222), S(140,232), S(144,236), S(146,244), S(150,256), S(154,260),
        S(170,266), S(188,272), S(198,280), S(216,314), S(224,316), S(226,322),
        S(236,348), S(238,354), S(246,382), S(256,398) }
    },
#endif
#ifdef ATOMIC
    {
      { S(-86,-77), S(-79,-64), S(-36,-40), S( -2,-24), S( 14,  8), S( 23, 25), // Knights
        S( 40, 26), S( 30, 23), S( 37, 29) },
      { S(-55,-65), S(-17,-34), S( 13, -9), S( 24, 20), S( 22, 25), S( 57, 39), // Bishops
        S( 32, 52), S( 66, 66), S( 51, 52), S( 56, 74), S( 73, 76), S( 85, 81),
        S( 93, 90), S(108, 87) },
      { S(-61,-73), S(-32,-28), S(-18,  9), S(-19, 30), S(-19, 57), S( 20, 78), // Rooks
        S( 12,104), S( 11,134), S( 21,133), S( 33,166), S( 35,168), S( 38,185),
        S( 25,172), S( 60,182), S( 58,155) },
      { S(-43,-43), S(-14,-16), S( -5,  1), S(  0, 23), S(  6, 24), S( 23, 59), // Queens
        S( 20, 55), S( 32, 66), S( 47, 89), S( 29, 77), S( 47, 89), S( 69,103),
        S( 63,110), S( 76,131), S( 73,115), S( 48,132), S( 58,131), S( 75,135),
        S( 82,122), S(111,130), S(114,144), S(101,139), S(106,146), S(107,167),
        S(115,157), S(129,175), S(132,176), S(130,210) }
    },
#endif
#ifdef CRAZYHOUSE
    {
      { S(-126, -96), S(-103,-31), S(-90,-27), S(-40,  3), S(  0,  3), S(  4,  0), // Knights
        S(  20,  12), S(  15, 33), S( 50, 46) },
      { S(-156, -79), S(-115,-43), S( 42,-14), S( 35, 26), S( 64, 26), S( 74, 38), // Bishops
        S(  70,  46), S(  83, 71), S( 70, 68), S( 66, 80), S( 64, 68), S( 70, 77),
        S(  97,  92), S(  89, 98) },
      { S( -53, -53), S( -22, -8), S(-48, 30), S(-14, 57), S( -4, 77), S( 11, 87), // Rooks
        S(   7, 115), S(  12,123), S( 27,120), S(  6,140), S( 55,156), S( 18,161),
        S(  51, 161), S(  54,171), S( 52,166) },
      { S( -26, -56), S( -24,-14), S(  7, 14), S(  8, 15), S( 18, 34), S( 14, 41), // Queens
        S(  28,  58), S(  33, 66), S( 40, 70), S( 47, 74), S( 50,100), S( 52,106),
        S(  59, 111), S(  50, 95), S( 60,115), S( 61,126), S( 75,144), S( 82,119),
        S(  95, 137), S( 102,138), S(100,142), S(119,154), S(129,156), S(107,156),
        S( 111, 177), S( 115,181), S(124,197), S(124,199) }
    },
#endif
#ifdef EXTINCTION
    {
      { S(-123, -90), S( -91,-32), S(-61,-29), S(-38,  3), S(  0,  3), S(  4,  0), // Knights
        S(  19,  12), S(  15, 33), S( 52, 45) },
      { S(-153, -80), S(-112,-41), S( 41,-14), S( 35, 24), S( 62, 26), S( 75, 41), // Bishops
        S(  72,  48), S(  85, 74), S( 74, 65), S( 66, 79), S( 64, 69), S( 73, 80),
        S( 107,  92), S(  96,101) },
      { S( -59, -51), S( -20, -8), S(-54, 32), S(-15, 54), S( -4, 70), S( 11, 84), // Rooks
        S(   6, 113), S(  13,123), S( 27,114), S(  6,144), S( 60,162), S( 19,162),
        S(  48, 170), S(  57,170), S( 52,177) },
      { S( -27, -56), S( -24,-14), S(  7, 13), S(  9, 16), S( 18, 37), S( 14, 40), // Queens
        S(  29,  56), S(  34, 64), S( 39, 73), S( 49, 65), S( 50, 98), S( 50,106),
        S(  60, 107), S(  53, 92), S( 62,119), S( 69,130), S( 77,145), S( 84,120),
        S(  90, 153), S(  98,131), S(106,139), S(116,147), S(127,157), S(112,154),
        S( 121, 174), S( 124,167), S(126,194), S(130,190) }
    },
#endif
#ifdef GRID
    {
      { S(-75,-76), S(-57,-54), S( -9,-28), S( -2,-10), S(  6,  5), S( 14, 12), // Knights
        S( 22, 26), S( 29, 29), S( 36, 29) },
      { S(-48,-59), S(-20,-23), S( 16, -3), S( 26, 13), S( 38, 24), S( 51, 42), // Bishops
        S( 55, 54), S( 63, 57), S( 63, 65), S( 68, 73), S( 81, 78), S( 81, 86),
        S( 91, 88), S( 98, 97) },
      { S(-58,-76), S(-27,-18), S(-15, 28), S(-10, 55), S( -5, 69), S( -2, 82), // Rooks
        S(  9,112), S( 16,118), S( 30,132), S( 29,142), S( 32,155), S( 38,165),
        S( 46,166), S( 48,169), S( 58,171) },
      { S(-39,-36), S(-21,-15), S(  3,  8), S(  3, 18), S( 14, 34), S( 22, 54), // Queens
        S( 28, 61), S( 41, 73), S( 43, 79), S( 48, 92), S( 56, 94), S( 60,104),
        S( 60,113), S( 66,120), S( 67,123), S( 70,126), S( 71,133), S( 73,136),
        S( 79,140), S( 88,143), S( 88,148), S( 99,166), S(102,170), S(102,175),
        S(106,184), S(109,191), S(113,206), S(116,212) }
    },
#endif
#ifdef HORDE
    {
      { S(-126,-90), S( -7,-22), S( -46,-25), S( 19,7), S( -53, 71), S( 31, -1), // Knights
        S(  -6, 51), S(-12, 47), S( -9, -56) },
      { S( -46,-2), S(30,66), S( 18, -27), S( 86, 21), S( 65, 11), S(147, 45), // Bishops
        S(  98, 38), S( 95, 52), S(122, 45), S( 95, 33), S( 89,103), S( 85, -9),
        S( 105, 70), S(131, 82) },
      { S( -56,-78), S(-25,-18), S(-11, 26), S( -5, 55), S( -4, 70), S( -1, 81), // Rooks
        S(   8,109), S( 14,120), S( 21,128), S( 23,143), S( 31,154), S( 32,160),
        S(  43,165), S( 49,168), S( 59,169) },
      { S( -40,-35), S(-25,-12), S(  2,  7), S(  4, 19), S( 14, 37), S( 24, 55), // Queens
        S(  25, 62), S( 40, 76), S( 43, 79), S( 47, 87), S( 54, 94), S( 56,102),
        S(  60,111), S( 70,116), S( 72,118), S( 73,122), S( 75,128), S( 77,130),
        S(  85,133), S( 94,136), S( 99,140), S(108,157), S(112,158), S(113,161),
        S( 118,174), S(119,177), S(123,191), S(128,199) }
    },
#endif
#ifdef KOTH
    {
      { S(-75,-76), S(-56,-54), S( -9,-26), S( -2,-10), S(  6,  5), S( 15, 11), // Knights
        S( 22, 26), S( 30, 28), S( 36, 29) },
      { S(-48,-58), S(-21,-19), S( 16, -2), S( 26, 12), S( 37, 22), S( 51, 42), // Bishops
        S( 54, 54), S( 63, 58), S( 65, 63), S( 71, 70), S( 79, 74), S( 81, 86),
        S( 92, 90), S( 97, 94) },
      { S(-56,-78), S(-25,-18), S(-11, 26), S( -5, 55), S( -4, 70), S( -1, 81), // Rooks
        S(  8,109), S( 14,120), S( 21,128), S( 23,143), S( 31,154), S( 32,160),
        S( 43,165), S( 49,168), S( 59,169) },
      { S(-40,-35), S(-25,-12), S(  2,  7), S(  4, 19), S( 14, 37), S( 24, 55), // Queens
        S( 25, 62), S( 40, 76), S( 43, 79), S( 47, 87), S( 54, 94), S( 56,102),
        S( 60,111), S( 70,116), S( 72,118), S( 73,122), S( 75,128), S( 77,130),
        S( 85,133), S( 94,136), S( 99,140), S(108,157), S(112,158), S(113,161),
        S(118,174), S(119,177), S(123,191), S(128,199) }
    },
#endif
#ifdef LOSERS
    {
      { S(-150,-152), S(-112,-108), S(-18,-52), S( -4,-20), S( 12, 10), S( 30, 22), // Knights
        S(  44,  52), S(  60,  56), S( 72, 58) },
      { S(-96,-116), S(-42,-38), S( 32, -4), S( 52, 24), S( 74, 44), S(102, 84), // Bishops
        S(108, 108), S(126,116), S(130,126), S(142,140), S(158,148), S(162,172),
        S(184, 180), S(194,188) },
      { S(-112,-156), S(-50,-36), S(-22, 52), S(-10,110), S( -8,140), S( -2,162), // Rooks
        S(  16, 218), S( 28,240), S( 42,256), S( 46,286), S( 62,308), S( 64,320),
        S(  86, 330), S( 98,336), S(118,338) },
      { S(-80,-70), S(-50,-24), S(  4, 14), S(  8, 38), S( 28, 74), S( 48,110), // Queens
        S( 50,124), S( 80,152), S( 86,158), S( 94,174), S(108,188), S(112,204),
        S(120,222), S(140,232), S(144,236), S(146,244), S(150,256), S(154,260),
        S(170,266), S(188,272), S(198,280), S(216,314), S(224,316), S(226,322),
        S(236,348), S(238,354), S(246,382), S(256,398) }
    },
#endif
#ifdef RACE
    {
      { S(-132,-117), S( -89,-110), S(-13,-49), S(-11,-15), S(-10,-30), S( 29, 17), // Knights
        S(  13,  32), S(  79,  69), S(109, 79) },
      { S(-101,-119), S( -19, -27), S( 27, -9), S( 35, 30), S( 62, 31), S(115, 72), // Bishops
        S(  91,  99), S( 138, 122), S(129,119), S(158,156), S(153,162), S(143,189),
        S( 172, 181), S( 196, 204) },
      { S(-131,-162), S( -57, -37), S( -8, 47), S( 12, 93), S(  3,127), S( 10,139), // Rooks
        S(   3, 240), S(  18, 236), S( 44,251), S( 44,291), S( 49,301), S( 67,316),
        S( 100, 324), S(  97, 340), S(110,324) },
      { S( -87, -68), S( -73,  -2), S( -7,  9), S( -5, 16), S( 39, 76), S( 39,118), // Queens
        S(  64, 131), S(  86, 169), S( 86,175), S( 78,166), S( 97,195), S(123,216),
        S( 137, 200), S( 155, 247), S(159,260), S(136,252), S(156,279), S(160,251),
        S( 165, 251), S( 194, 267), S(204,271), S(216,331), S(226,304), S(223,295),
        S( 239, 316), S( 228, 365), S(240,385), S(249,377) }
    },
#endif
#ifdef THREECHECK
    {
      { S(-74,-76), S(-55,-54), S( -9,-26), S( -2,-10), S(  6,  5), S( 15, 11), // Knights
        S( 22, 26), S( 31, 27), S( 37, 29) },
      { S(-49,-56), S(-23,-18), S( 15, -2), S( 25, 12), S( 36, 22), S( 50, 42), // Bishops
        S( 53, 54), S( 64, 57), S( 67, 63), S( 71, 68), S( 84, 76), S( 79, 87),
        S( 95, 91), S( 98, 93) },
      { S(-57,-76), S(-25,-18), S(-11, 25), S( -5, 53), S( -4, 70), S( -1, 78), // Rooks
        S(  8,111), S( 14,116), S( 22,125), S( 24,148), S( 31,159), S( 31,173),
        S( 44,163), S( 50,162), S( 56,168) },
      { S(-42,-35), S(-25,-12), S(  2,  7), S(  4, 19), S( 14, 37), S( 24, 53), // Queens
        S( 26, 63), S( 39, 80), S( 42, 77), S( 48, 88), S( 53, 96), S( 57, 96),
        S( 61,108), S( 71,116), S( 70,116), S( 74,125), S( 75,133), S( 78,133),
        S( 85,137), S( 97,135), S(103,141), S(107,165), S(109,153), S(115,162),
        S(119,164), S(121,184), S(121,192), S(131,203) }
    },
#endif
#ifdef TWOKINGS
    {
      { S(-75,-76), S(-57,-54), S( -9,-28), S( -2,-10), S(  6,  5), S( 14, 12), // Knights
        S( 22, 26), S( 29, 29), S( 36, 29) },
      { S(-48,-59), S(-20,-23), S( 16, -3), S( 26, 13), S( 38, 24), S( 51, 42), // Bishops
        S( 55, 54), S( 63, 57), S( 63, 65), S( 68, 73), S( 81, 78), S( 81, 86),
        S( 91, 88), S( 98, 97) },
      { S(-58,-76), S(-27,-18), S(-15, 28), S(-10, 55), S( -5, 69), S( -2, 82), // Rooks
        S(  9,112), S( 16,118), S( 30,132), S( 29,142), S( 32,155), S( 38,165),
        S( 46,166), S( 48,169), S( 58,171) },
      { S(-39,-36), S(-21,-15), S(  3,  8), S(  3, 18), S( 14, 34), S( 22, 54), // Queens
        S( 28, 61), S( 41, 73), S( 43, 79), S( 48, 92), S( 56, 94), S( 60,104),
        S( 60,113), S( 66,120), S( 67,123), S( 70,126), S( 71,133), S( 73,136),
        S( 79,140), S( 88,143), S( 88,148), S( 99,166), S(102,170), S(102,175),
        S(106,184), S(109,191), S(113,206), S(116,212) }
    },
#endif
  };

  // BishopPawns[distance from edge] contains a file-dependent penalty for pawns on
  // squares of the same color as our bishop.
  constexpr Score BishopPawns[int(FILE_NB) / 2] = {
    S(3, 8), S(3, 9), S(2, 8), S(3, 8)
  };

  // KingProtector[knight/bishop] contains penalty for each distance unit to own king
  constexpr Score KingProtector[] = { S(8, 9), S(6, 9) };

  // Outpost[knight/bishop] contains bonuses for each knight or bishop occupying a
  // pawn protected square on rank 4 to 6 which is also safe from a pawn attack.
  constexpr Score Outpost[] = { S(57, 38), S(31, 24) };

  // PassedRank[Rank] contains a bonus according to the rank of a passed pawn
  constexpr Score PassedRank[VARIANT_NB][RANK_NB] = {
    {
    S(0, 0), S(7, 27), S(16, 32), S(17, 40), S(64, 71), S(170, 174), S(278, 262)
    },
#ifdef ANTI
    { S(0, 0), S(5, 7), S(5, 14), S(31, 38), S(73, 73), S(166, 166), S(252, 252) },
#endif
#ifdef ATOMIC
    { S(0, 0), S(95, 86), S(118, 43), S(94, 61), S(142, 62), S(196, 150), S(204, 256) },
#endif
#ifdef CRAZYHOUSE
    { S(0, 0), S(15, 27), S(23, 13), S(13, 19), S(88, 111), S(177, 140), S(229, 293) },
#endif
#ifdef EXTINCTION
    { S(0, 0), S(5, 7), S(5, 14), S(31, 38), S(73, 73), S(166, 166), S(252, 252) },
#endif
#ifdef GRID
    { S(0, 0), S(11, 2), S(4, 0), S(27, 34), S(58, 17), S(168, 165), S(251, 253) },
#endif
#ifdef HORDE
    { S(0, 0), S(-66, 10), S(-25, 7), S(66, -12), S(68, 81), S(72, 210), S(250, 258) },
#endif
#ifdef KOTH
    { S(0, 0), S(5, 7), S(5, 14), S(31, 38), S(73, 73), S(166, 166), S(252, 252) },
#endif
#ifdef LOSERS
    { S(0, 0), S(5, 8), S(5, 13), S(31, 36), S(72, 72), S(170, 159), S(276, 251) },
#endif
#ifdef RACE
    {},
#endif
#ifdef THREECHECK
    { S(0, 0), S(5, 7), S(5, 14), S(31, 38), S(73, 73), S(166, 166), S(252, 252) },
#endif
#ifdef TWOKINGS
    { S(0, 0), S(5, 7), S(5, 14), S(31, 38), S(73, 73), S(166, 166), S(252, 252) },
#endif
  };

  constexpr Score RookOnClosedFile = S(10, 5);
  constexpr Score RookOnOpenFile[] = { S(19, 6), S(47, 26) };

  // ThreatByMinor/ByRook[attacked PieceType] contains bonuses according to
  // which piece type attacks which one. Attacks on lesser pieces which are
  // pawn-defended are not considered.
  constexpr Score ThreatByMinor[PIECE_TYPE_NB] = {
    S(0, 0), S(5, 32), S(55, 41), S(77, 56), S(89, 119), S(79, 162)
  };

  constexpr Score ThreatByRook[PIECE_TYPE_NB] = {
    S(0, 0), S(3, 44), S(37, 68), S(42, 60), S(0, 39), S(58, 43)
  };

  constexpr Value CorneredBishop = Value(50);

  // Assorted bonuses and penalties
#ifdef ATOMIC
  constexpr Score AtomicConfinedKing = S(104, 97);
  constexpr Score ThreatByBlast      = S( 84, 78);
#endif
#ifdef HORDE
  constexpr Score HordeShelter = S(71, 61);
#endif
#ifdef KOTH
  constexpr Score KothDistanceBonus[6] = {
    S(1949, 1934), S(454, 364), S(151, 158), S(75, 85), S(42, 49), S(0, 0)
  };
  constexpr Score KothSafeCenter = S(163, 207);
#endif
#ifdef ANTI
  constexpr Score PieceCountAnti    = S(119, 123);
  constexpr Score ThreatsAnti[]     = { S(192, 203), S(411, 322) };
  constexpr Score AttacksAnti[2][2][PIECE_TYPE_NB] = {
    {
      { S( 30, 141), S( 26,  94), S(161, 105), S( 70, 123), S( 61,  72), S( 78, 12), S(139, 115) },
      { S( 56,  89), S( 82, 107), S(114,  93), S(110, 115), S(188, 112), S( 73, 59), S(122,  59) }
    },
    {
      { S(119, 142), S( 99, 105), S(123, 193), S(142,  37), S(118,  96), S( 50, 12), S( 91,  85) },
      { S( 58,  81), S( 66, 110), S(105, 153), S(100, 143), S(140, 113), S(145, 73), S(153, 154) }
    }
  };
#endif
#ifdef LOSERS
  constexpr Score ThreatsLosers[]     = { S(216, 279), S(441, 341) };
  constexpr Score AttacksLosers[2][2][PIECE_TYPE_NB] = {
    {
      { S( 27, 140), S( 23,  95), S(160, 112), S( 78, 129), S( 65,  75), S( 70, 13), S(146, 123) },
      { S( 58,  82), S( 80, 112), S(124,  87), S(103, 110), S(185, 107), S( 72, 60), S(126,  62) }
    },
    {
      { S(111, 127), S(102,  95), S(121, 183), S(140,  37), S(120,  99), S( 55, 11), S( 88,  93) },
      { S( 56,  69), S( 72, 124), S(109, 154), S( 98, 149), S(129, 113), S(147, 72), S(157, 152) }
    }
  };
#endif
#ifdef CRAZYHOUSE
  constexpr int KingDangerInHand[PIECE_TYPE_NB] = {
    79, 16, 200, 61, 138, 152
  };
#endif
#ifdef RACE
  // Bonus for distance of king from 8th rank
  constexpr Score KingRaceBonus[RANK_NB] = {
    S(14282, 14493), S(6369, 5378), S(4224, 3557), S(2633, 2219),
    S( 1614,  1456), S( 975,  885), S( 528,  502), S(   0,    0)
  };
#endif
  constexpr Score UncontestedOutpost  = S(  1, 10);
  constexpr Score BishopOnKingRing    = S( 24,  0);
  constexpr Score BishopXRayPawns     = S(  4,  5);
  constexpr Score FlankAttacks[VARIANT_NB] = {
    S( 8,  0),
#ifdef ANTI
    S( 0,  0),
#endif
#ifdef ATOMIC
    S(17,  0),
#endif
#ifdef CRAZYHOUSE
    S(14, 20),
#endif
#ifdef EXTINCTION
    S( 0,  0),
#endif
#ifdef GRID
    S( 7,  0),
#endif
#ifdef HORDE
    S( 7,  0),
#endif
#ifdef KOTH
    S( 7,  0),
#endif
#ifdef LOSERS
    S( 7,  0),
#endif
#ifdef RACE
    S( 0,  0),
#endif
#ifdef THREECHECK
    S(16,  9),
#endif
#ifdef TWOKINGS
    S( 7,  0),
#endif
  };
  constexpr Score Hanging             = S( 69, 36);
  constexpr Score KnightOnQueen       = S( 16, 11);
  constexpr Score LongDiagonalBishop  = S( 45,  0);
  constexpr Score MinorBehindPawn     = S( 18,  3);
  constexpr Score PassedFile          = S( 11,  8);
  constexpr Score PawnlessFlank       = S( 17, 95);
  constexpr Score ReachableOutpost    = S( 31, 22);
  constexpr Score RestrictedPiece     = S(  7,  7);
  constexpr Score RookOnKingRing      = S( 16,  0);
  constexpr Score SliderOnQueen       = S( 60, 18);
  constexpr Score ThreatByKing        = S( 24, 89);
  constexpr Score ThreatByPawnPush    = S( 48, 39);
  constexpr Score ThreatBySafePawn    = S(173, 94);
  constexpr Score TrappedRook         = S( 55, 13);
  constexpr Score WeakQueenProtection = S( 14,  0);
  constexpr Score WeakQueen           = S( 56, 15);


#undef S

  // Evaluation class computes and stores attacks tables and other working data
  template<Tracing T>
  class Evaluation {

  public:
    Evaluation() = delete;
    explicit Evaluation(const Position& p) : pos(p) {}
    Evaluation& operator=(const Evaluation&) = delete;
    Value value();
    Value variantValue(Value v);

  private:
    template<Color Us> void initialize();
    template<Color Us, PieceType Pt> Score pieces();
    template<Color Us> Score king() const;
    template<Color Us> Score threats() const;
    template<Color Us> Score passed() const;
    template<Color Us> Score space() const;
    template<Color Us> Score variant() const;
    Value winnable(Score score) const;

    const Position& pos;
    Material::Entry* me;
    Pawns::Entry* pe;
    Bitboard mobilityArea[COLOR_NB];
    Score mobility[COLOR_NB] = { SCORE_ZERO, SCORE_ZERO };

    // attackedBy[color][piece type] is a bitboard representing all squares
    // attacked by a given color and piece type. Special "piece types" which
    // is also calculated is ALL_PIECES.
    Bitboard attackedBy[COLOR_NB][PIECE_TYPE_NB];

    // attackedBy2[color] are the squares attacked by at least 2 units of a given
    // color, including x-rays. But diagonal x-rays through pawns are not computed.
    Bitboard attackedBy2[COLOR_NB];

    // kingRing[color] are the squares adjacent to the king plus some other
    // very near squares, depending on king position.
    Bitboard kingRing[COLOR_NB];

    // kingAttackersCount[color] is the number of pieces of the given color
    // which attack a square in the kingRing of the enemy king.
    int kingAttackersCount[COLOR_NB];

    // kingAttackersWeight[color] is the sum of the "weights" of the pieces of
    // the given color which attack a square in the kingRing of the enemy king.
    // The weights of the individual piece types are given by the elements in
    // the KingAttackWeights array.
    int kingAttackersWeight[COLOR_NB];

    // kingAttacksCount[color] is the number of attacks by the given color to
    // squares directly adjacent to the enemy king. Pieces which attack more
    // than one square are counted multiple times. For instance, if there is
    // a white knight on g5 and black's king is on g8, this white knight adds 2
    // to kingAttacksCount[WHITE].
    int kingAttacksCount[COLOR_NB];
  };


  // Evaluation::initialize() computes king and pawn attacks, and the king ring
  // bitboard for a given color. This is done at the beginning of the evaluation.

  template<Tracing T> template<Color Us>
  void Evaluation<T>::initialize() {

    constexpr Color     Them = ~Us;
    constexpr Direction Up   = pawn_push(Us);
    constexpr Direction Down = -Up;
    constexpr Bitboard LowRanks = (Us == WHITE ? Rank2BB | Rank3BB : Rank7BB | Rank6BB);

#ifdef HORDE
    const Square ksq = (pos.is_horde() && pos.is_horde_color(Us)) ? SQ_NONE : pos.square<KING>(Us);
#else
    const Square ksq = pos.square<KING>(Us);
#endif

    Bitboard dblAttackByPawn = pawn_double_attacks_bb<Us>(pos.pieces(Us, PAWN));

    // Find our pawns that are blocked or on the first two ranks
    Bitboard b = pos.pieces(Us, PAWN) & (shift<Down>(pos.pieces()) | LowRanks);

    // Squares occupied by those pawns, by our king or queen, by blockers to attacks on our king
    // or controlled by enemy pawns are excluded from the mobility area.
#ifdef ANTI
    if (pos.is_anti())
        mobilityArea[Us] = ~b;
    else
#endif
#ifdef HORDE
    if (pos.is_horde() && pos.is_horde_color(Us))
        mobilityArea[Us] = ~(b | pe->pawn_attacks(Them));
    else
#endif
    mobilityArea[Us] = ~(b | pos.pieces(Us, KING, QUEEN) | pos.blockers_for_king(Us) | pe->pawn_attacks(Them));

    // Initialize attackedBy[] for king and pawns
#ifdef PLACEMENT
    if (pos.is_placement() && pos.count_in_hand<KING>(Us))
        attackedBy[Us][KING] = 0;
    else
#endif
    switch (pos.variant())
    {
#ifdef ANTI
    case ANTI_VARIANT:
        attackedBy[Us][KING] = 0;
        for (Bitboard kings = pos.pieces(Us, KING); kings; )
            attackedBy[Us][KING] |= attacks_bb<KING>(pop_lsb(kings));
    break;
#endif
#ifdef EXTINCTION
    case EXTINCTION_VARIANT:
        attackedBy[Us][KING] = 0;
        for (Bitboard kings = pos.pieces(Us, KING); kings; )
            attackedBy[Us][KING] |= attacks_bb<KING>(pop_lsb(kings));
    break;
#endif
#ifdef HORDE
    case HORDE_VARIANT:
        if (pos.is_horde_color(Us))
        {
            attackedBy[Us][KING] = 0;
            break;
        }
    [[fallthrough]];
#endif
    default:
    attackedBy[Us][KING] = attacks_bb<KING>(ksq);
    }
    attackedBy[Us][PAWN] = pe->pawn_attacks(Us);
    attackedBy[Us][ALL_PIECES] = attackedBy[Us][KING] | attackedBy[Us][PAWN];
    attackedBy2[Us] = dblAttackByPawn | (attackedBy[Us][KING] & attackedBy[Us][PAWN]);

    // Init our king safety tables
#ifdef PLACEMENT
    if (pos.is_placement() && pos.count_in_hand<KING>(Us))
        kingRing[Us] = 0;
    else
#endif
    switch (pos.variant())
    {
#ifdef ANTI
    case ANTI_VARIANT:
        kingRing[Us] = 0;
    break;
#endif
#ifdef EXTINCTION
    case EXTINCTION_VARIANT:
        kingRing[Us] = 0;
    break;
#endif
#ifdef HORDE
    case HORDE_VARIANT:
        if (pos.is_horde_color(Us))
        {
            kingRing[Us] = 0;
            break;
        }
    [[fallthrough]];
#endif
    default:
    Square s = make_square(std::clamp(file_of(ksq), FILE_B, FILE_G),
                           std::clamp(rank_of(ksq), RANK_2, RANK_7));
    kingRing[Us] = attacks_bb<KING>(s) | s;
    }

    kingAttackersCount[Them] = popcount(kingRing[Us] & pe->pawn_attacks(Them));
    kingAttacksCount[Them] = kingAttackersWeight[Them] = 0;

    // Remove from kingRing[] the squares defended by two pawns
    kingRing[Us] &= ~dblAttackByPawn;
  }


  // Evaluation::pieces() scores pieces of a given color and type

  template<Tracing T> template<Color Us, PieceType Pt>
  Score Evaluation<T>::pieces() {

    constexpr Color     Them = ~Us;
    constexpr Direction Down = -pawn_push(Us);
    constexpr Bitboard OutpostRanks = (Us == WHITE ? Rank4BB | Rank5BB | Rank6BB
                                                   : Rank5BB | Rank4BB | Rank3BB);
    Bitboard b1 = pos.pieces(Us, Pt);
    Bitboard b, bb;
    Score score = SCORE_ZERO;

    attackedBy[Us][Pt] = 0;

    while (b1)
    {
        Square s = pop_lsb(b1);

        // Find attacked squares, including x-ray attacks for bishops and rooks
        b = Pt == BISHOP ? attacks_bb<BISHOP>(s, pos.pieces() ^ pos.pieces(QUEEN))
          : Pt ==   ROOK ? attacks_bb<  ROOK>(s, pos.pieces() ^ pos.pieces(QUEEN) ^ pos.pieces(Us, ROOK))
                         : attacks_bb<Pt>(s, pos.pieces());

#ifdef GRID
        if (pos.is_grid())
            b &= ~pos.grid_bb(s);
#endif
        if (pos.blockers_for_king(Us) & s)
            b &= line_bb(pos.square<KING>(Us), s);

        attackedBy2[Us] |= attackedBy[Us][ALL_PIECES] & b;
        attackedBy[Us][Pt] |= b;
        attackedBy[Us][ALL_PIECES] |= b;

        if (b & kingRing[Them])
        {
            kingAttackersCount[Us]++;
            kingAttackersWeight[Us] += KingAttackWeights[pos.variant()][Pt];
            kingAttacksCount[Us] += popcount(b & attackedBy[Them][KING]);
        }

        else if (Pt == ROOK && (file_bb(s) & kingRing[Them]))
            score += RookOnKingRing;

        else if (Pt == BISHOP && (attacks_bb<BISHOP>(s, pos.pieces(PAWN)) & kingRing[Them]))
            score += BishopOnKingRing;

        int mob = popcount(b & mobilityArea[Us]);
#ifdef ANTI
        if (pos.is_anti())
            continue;
#endif
#ifdef HORDE
        if (pos.is_horde() && pos.is_horde_color(Us))
            continue;
#endif
#ifdef PLACEMENT
        if (pos.is_placement() && pos.count_in_hand<KING>(Us))
            continue;
#endif
#ifdef LOSERS
        if (pos.is_losers())
            continue;
#endif
        mobility[Us] += MobilityBonus[pos.variant()][Pt - 2][mob];

        if (Pt == BISHOP || Pt == KNIGHT)
        {
            // Bonus if the piece is on an outpost square or can reach one
            // Bonus for knights (UncontestedOutpost) if few relevant targets
            bb = OutpostRanks & (attackedBy[Us][PAWN] | shift<Down>(pos.pieces(PAWN)))
                              & ~pe->pawn_attacks_span(Them);
            Bitboard targets = pos.pieces(Them) & ~pos.pieces(PAWN);

            if (   Pt == KNIGHT
                && bb & s & ~CenterFiles // on a side outpost
                && !(b & targets)        // no relevant attacks
                && (!more_than_one(targets & (s & QueenSide ? QueenSide : KingSide))))
                score += UncontestedOutpost * popcount(pos.pieces(PAWN) & (s & QueenSide ? QueenSide : KingSide));
            else if (bb & s)
                score += Outpost[Pt == BISHOP];
            else if (Pt == KNIGHT && bb & b & ~pos.pieces(Us))
                score += ReachableOutpost;

            // Bonus for a knight or bishop shielded by pawn
            if (shift<Down>(pos.pieces(PAWN)) & s)
                score += MinorBehindPawn;

            // Penalty if the piece is far from the king
            score -= KingProtector[Pt == BISHOP] * distance(pos.square<KING>(Us), s);

            if constexpr (Pt == BISHOP)
            {
                // Penalty according to the number of our pawns on the same color square as the
                // bishop, bigger when the center files are blocked with pawns and smaller
                // when the bishop is outside the pawn chain.
                Bitboard blocked = pos.pieces(Us, PAWN) & shift<Down>(pos.pieces());

                score -= BishopPawns[edge_distance(file_of(s))] * pos.pawns_on_same_color_squares(Us, s)
                                     * (!(attackedBy[Us][PAWN] & s) + popcount(blocked & CenterFiles));

                // Penalty for all enemy pawns x-rayed
                score -= BishopXRayPawns * popcount(attacks_bb<BISHOP>(s) & pos.pieces(Them, PAWN));

                // Bonus for bishop on a long diagonal which can "see" both center squares
                if (more_than_one(attacks_bb<BISHOP>(s, pos.pieces(PAWN)) & Center))
                    score += LongDiagonalBishop;

                // An important Chess960 pattern: a cornered bishop blocked by a friendly
                // pawn diagonally in front of it is a very serious problem, especially
                // when that pawn is also blocked.
                if (   pos.is_chess960()
                    && (s == relative_square(Us, SQ_A1) || s == relative_square(Us, SQ_H1)))
                {
                    Direction d = pawn_push(Us) + (file_of(s) == FILE_A ? EAST : WEST);
                    if (pos.piece_on(s + d) == make_piece(Us, PAWN))
                        score -= !pos.empty(s + d + pawn_push(Us)) ? 4 * make_score(CorneredBishop, CorneredBishop)
                                                                   : 3 * make_score(CorneredBishop, CorneredBishop);
                }
            }
        }

        if constexpr (Pt == ROOK)
        {
            // Bonuses for rook on a (semi-)open or closed file
            if (pos.is_on_semiopen_file(Us, s))
            {
                score += RookOnOpenFile[pos.is_on_semiopen_file(Them, s)];
            }
            else
            {
                // If our pawn on this file is blocked, increase penalty
                if ( pos.pieces(Us, PAWN)
                   & shift<Down>(pos.pieces())
                   & file_bb(s))
                {
                    score -= RookOnClosedFile;
                }

                // Penalty when trapped by the king, even more if the king cannot castle
                if (mob <= 3)
                {
                    File kf = file_of(pos.square<KING>(Us));
                    if ((kf < FILE_E) == (file_of(s) < kf))
                        score -= TrappedRook * (1 + !pos.castling_rights(Us));
                }
            }
        }

        if constexpr (Pt == QUEEN)
        {
            // Penalty if any relative pin or discovered attack against the queen
            Bitboard queenPinners;
            if (pos.slider_blockers(pos.pieces(Them, ROOK, BISHOP), s, queenPinners))
                score -= WeakQueen;
        }
    }
    if constexpr (T)
        Trace::add(Pt, Us, score);

    return score;
  }


  // Evaluation::king() assigns bonuses and penalties to a king of a given color

  template<Tracing T> template<Color Us>
  Score Evaluation<T>::king() const {

#ifdef ANTI
    if (pos.is_anti())
        return SCORE_ZERO;
#endif
#ifdef EXTINCTION
    if (pos.is_extinction())
        return SCORE_ZERO;
#endif
#ifdef HORDE
    if (pos.is_horde() && pos.is_horde_color(Us))
        return SCORE_ZERO;
#endif
#ifdef PLACEMENT
    if (pos.is_placement() && pos.count_in_hand<KING>(Us))
        return SCORE_ZERO;
#endif

    constexpr Color    Them = ~Us;
    constexpr Bitboard Camp = (Us == WHITE ? AllSquares ^ Rank6BB ^ Rank7BB ^ Rank8BB
                                           : AllSquares ^ Rank1BB ^ Rank2BB ^ Rank3BB);

    Bitboard weak, b1, b2, b3, safe, unsafeChecks = 0;
    Bitboard rookChecks, queenChecks, bishopChecks, knightChecks;
    int kingDanger = 0;
    const Square ksq = pos.square<KING>(Us);

    // Init the score with king shelter and enemy pawns storm
    Score score = pe->king_safety<Us>(pos);

    // Attacked squares defended at most once by our queen or king
#ifdef ATOMIC
    if (pos.is_atomic())
        weak =  (attackedBy[Them][ALL_PIECES] ^ attackedBy[Them][KING])
              & ~(attackedBy[Us][ALL_PIECES] ^ attackedBy[Us][KING]);
    else
#endif
    weak =  attackedBy[Them][ALL_PIECES]
          & ~attackedBy2[Us]
          & (~attackedBy[Us][ALL_PIECES] | attackedBy[Us][KING] | attackedBy[Us][QUEEN]);

    Bitboard h = 0;
#ifdef CRAZYHOUSE
    if (pos.is_house())
        h = pos.count_in_hand<QUEEN>(Them) ? weak & ~pos.pieces() : 0;
#endif

    // Analyse the safe enemy's checks which are possible on next move
    safe  = ~pos.pieces(Them);
#ifdef ATOMIC
    if (pos.is_atomic())
        safe &= ~pos.pieces(Us) | attackedBy2[Them];
    else
#endif
    safe &= ~attackedBy[Us][ALL_PIECES] | (weak & attackedBy2[Them]);

    b1 = attacks_bb<ROOK  >(ksq, pos.pieces() ^ pos.pieces(Us, QUEEN));
    b2 = attacks_bb<BISHOP>(ksq, pos.pieces() ^ pos.pieces(Us, QUEEN));

    Bitboard dqko = ~attackedBy2[Us] & (attackedBy[Us][QUEEN] | attackedBy[Us][KING]);
    Bitboard dropSafe = (safe | (attackedBy[Them][ALL_PIECES] & dqko)) & ~pos.pieces(Us);

    // Enemy rooks checks
#ifdef CRAZYHOUSE
    h = pos.is_house() && pos.count_in_hand<ROOK>(Them) ? ~pos.pieces() : 0;
#endif
    rookChecks = b1 & (attackedBy[Them][ROOK] | (h & dropSafe)) & safe;
    if (rookChecks)
        kingDanger += SafeCheck[ROOK][more_than_one(rookChecks)];
    else
        unsafeChecks |= b1 & (attackedBy[Them][ROOK] | h);

    // Enemy queen safe checks: count them only if the checks are from squares from
    // which opponent cannot give a rook check, because rook checks are more valuable.
#ifdef CRAZYHOUSE
    h = pos.is_house() && pos.count_in_hand<QUEEN>(Them) ? ~pos.pieces() : 0;
#endif
    queenChecks =  (b1 | b2) & (attackedBy[Them][QUEEN] | (h & dropSafe)) & safe
                 & ~(attackedBy[Us][QUEEN] | rookChecks);
    if (queenChecks)
        kingDanger += SafeCheck[QUEEN][more_than_one(queenChecks)];

    // Enemy bishops checks: count them only if they are from squares from which
    // opponent cannot give a queen check, because queen checks are more valuable.
#ifdef CRAZYHOUSE
    h = pos.is_house() && pos.count_in_hand<BISHOP>(Them) ? ~pos.pieces() : 0;
#endif
    bishopChecks =  b2 & (attackedBy[Them][BISHOP] | (h & dropSafe)) & safe
                  & ~queenChecks;
    if (bishopChecks)
        kingDanger += SafeCheck[BISHOP][more_than_one(bishopChecks)];

    else
        unsafeChecks |= b2 & (attackedBy[Them][BISHOP] | (h & dropSafe));

    // Enemy knights checks
#ifdef CRAZYHOUSE
    h = pos.is_house() && pos.count_in_hand<KNIGHT>(Them) ? ~pos.pieces() : 0;
#endif
    knightChecks = attacks_bb<KNIGHT>(ksq) & (attackedBy[Them][KNIGHT] | (h & dropSafe));
    if (knightChecks & safe)
        kingDanger += SafeCheck[KNIGHT][more_than_one(knightChecks & (safe | (h & dropSafe)))];
    else
        unsafeChecks |= knightChecks & (attackedBy[Them][KNIGHT] | h);

#ifdef CRAZYHOUSE
    // Enemy pawn checks
    if (pos.is_house())
    {
        constexpr Direction Down = pawn_push(Them);
        Bitboard pawnChecks = pawn_attacks_bb<Us>(ksq);
        h = pos.count_in_hand<PAWN>(Them) ? ~pos.pieces() : 0;
        Bitboard pawnMoves = (attackedBy[Them][PAWN] & pos.pieces(Us)) | (shift<Down>(pos.pieces(Them, PAWN)) & ~pos.pieces());
        if (pawnChecks & ((pawnMoves & safe) | (h & dropSafe)))
            kingDanger += SafeCheck[PAWN][more_than_one(pawnChecks & (safe | (h & dropSafe)))];
        else
            unsafeChecks |= pawnChecks & (pawnMoves | h);
    }
#endif
#ifdef RACE
    if (pos.is_race())
    {
        kingDanger = -kingDanger;
        int s = relative_rank(BLACK, ksq);
        Bitboard b = file_bb(ksq);
        for (Rank kr = rank_of(ksq), r = Rank(kr + 1); r <= RANK_8; ++r)
        {
            // Pinned piece attacks are not included in attackedBy
            b |= shift<EAST>(b) | shift<WEST>(b);
            if (!(rank_bb(r) & b & ~attackedBy[Them][ALL_PIECES]))
                s++;
        }
        score += KingRaceBonus[std::min(s, 7)];
    }
#endif

    // Find the squares that opponent attacks in our king flank, the squares
    // which they attack twice in that flank, and the squares that we defend.
    b1 = attackedBy[Them][ALL_PIECES] & KingFlank[file_of(ksq)] & Camp;
    b2 = b1 & attackedBy2[Them];
    b3 = attackedBy[Us][ALL_PIECES] & KingFlank[file_of(ksq)] & Camp;

    int kingFlankAttack  = popcount(b1) + popcount(b2);
    int kingFlankDefense = popcount(b3);

    const auto KDP = KingDangerParams[pos.variant()];
    kingDanger +=        kingAttackersCount[Them] * kingAttackersWeight[Them] // (~10 Elo)
                 + KDP[0] * popcount(kingRing[Us] & weak)                     // (~15 Elo)
                 + KDP[1] * popcount(unsafeChecks)                            // (~4 Elo)
                 + KDP[2] * popcount(pos.blockers_for_king(Us))               // (~2 Elo)
                 + KDP[3] * kingAttacksCount[Them]                            // (~0.5 Elo)
                 + KDP[4] * kingFlankAttack * kingFlankAttack / 8             // (~0.5 Elo)
                 +       mg_value(mobility[Them] - mobility[Us])              // (~0.5 Elo)
                 + KDP[5] * !pos.count<QUEEN>(Them)                              // (~24 Elo)
                 + KDP[6] * bool(attackedBy[Us][KNIGHT] & attackedBy[Us][KING])  // (~5 Elo)
                 + KDP[7] * mg_value(score) / 8                                  // (~8 Elo)
                 + KDP[8] * kingFlankDefense                                     // (~5 Elo)
                 + KDP[9];                                                       // (~0.5 Elo)
#ifdef CRAZYHOUSE
    if (pos.is_house())
    {
        kingDanger += KingDangerInHand[ALL_PIECES] * pos.count_in_hand<ALL_PIECES>(Them);
        kingDanger += KingDangerInHand[PAWN] * pos.count_in_hand<PAWN>(Them);
        kingDanger += KingDangerInHand[KNIGHT] * pos.count_in_hand<KNIGHT>(Them);
        kingDanger += KingDangerInHand[BISHOP] * pos.count_in_hand<BISHOP>(Them);
        kingDanger += KingDangerInHand[ROOK] * pos.count_in_hand<ROOK>(Them);
        kingDanger += KingDangerInHand[QUEEN] * pos.count_in_hand<QUEEN>(Them);
        h = pos.count_in_hand<QUEEN>(Them) ? weak & ~pos.pieces() : 0;
    }
#endif

    // Transform the kingDanger units into a Score, and subtract it from the evaluation
    if (kingDanger > 100)
    {
        int v = kingDanger * kingDanger / 4096;
#ifdef CRAZYHOUSE
        if (pos.is_house() && Us == pos.side_to_move())
            v -= v / 10;
        if (pos.is_house())
            v = std::min(v, (int)QueenValueMg);
#endif
        score -= make_score(v, kingDanger / 16 + KDP[10] * v / 256);
    }

    // Penalty when our king is on a pawnless flank
    if (!(pos.pieces(PAWN) & KingFlank[file_of(ksq)]))
        score -= PawnlessFlank;

    // Penalty if king flank is under attack, potentially moving toward the king
    score -= FlankAttacks[pos.variant()] * kingFlankAttack;

    if constexpr (T)
        Trace::add(KING, Us, score);

    return score;
  }


  // Evaluation::threats() assigns bonuses according to the types of the
  // attacking and the attacked pieces.

  template<Tracing T> template<Color Us>
  Score Evaluation<T>::threats() const {

    constexpr Color     Them     = ~Us;
    constexpr Direction Up       = pawn_push(Us);
    constexpr Bitboard  TRank3BB = (Us == WHITE ? Rank3BB : Rank6BB);

    Bitboard b, weak, defended, nonPawnEnemies, stronglyProtected, safe;
    Score score = SCORE_ZERO;
#ifdef ANTI
    if (pos.is_anti()) {} else
#endif
#ifdef ATOMIC
    if (pos.is_atomic()) {} else
#endif
#ifdef GRID
    if (pos.is_grid()) {} else
#endif
#ifdef LOSERS
    if (pos.is_losers()) {} else
#endif
    {
    // Non-pawn enemies
    nonPawnEnemies = pos.pieces(Them) & ~pos.pieces(PAWN);

    // Squares strongly protected by the enemy, either because they defend the
    // square with a pawn, or because they defend the square twice and we don't.
    stronglyProtected =  attackedBy[Them][PAWN]
                       | (attackedBy2[Them] & ~attackedBy2[Us]);

    // Non-pawn enemies, strongly protected
    defended = nonPawnEnemies & stronglyProtected;

    // Enemies not strongly protected and under our attack
    weak = pos.pieces(Them) & ~stronglyProtected & attackedBy[Us][ALL_PIECES];

    // Bonus according to the kind of attacking pieces
    if (defended | weak)
    {
        b = (defended | weak) & (attackedBy[Us][KNIGHT] | attackedBy[Us][BISHOP]);
        while (b)
            score += ThreatByMinor[type_of(pos.piece_on(pop_lsb(b)))];

        b = weak & attackedBy[Us][ROOK];
        while (b)
            score += ThreatByRook[type_of(pos.piece_on(pop_lsb(b)))];

        if (weak & attackedBy[Us][KING])
            score += ThreatByKing;

        b =  ~attackedBy[Them][ALL_PIECES]
           | (nonPawnEnemies & attackedBy2[Us]);
        score += Hanging * popcount(weak & b);

        // Additional bonus if weak piece is only protected by a queen
        score += WeakQueenProtection * popcount(weak & attackedBy[Them][QUEEN]);
    }

    // Bonus for restricting their piece moves
    b =   attackedBy[Them][ALL_PIECES]
       & ~stronglyProtected
       &  attackedBy[Us][ALL_PIECES];
    score += RestrictedPiece * popcount(b);

    // Protected or unattacked squares
    safe = ~attackedBy[Them][ALL_PIECES] | attackedBy[Us][ALL_PIECES];

    // Bonus for attacking enemy pieces with our relatively safe pawns
    b = pos.pieces(Us, PAWN) & safe;
    b = pawn_attacks_bb<Us>(b) & nonPawnEnemies;
    score += ThreatBySafePawn * popcount(b);

    // Find squares where our pawns can push on the next move
    b  = shift<Up>(pos.pieces(Us, PAWN)) & ~pos.pieces();
    b |= shift<Up>(b & TRank3BB) & ~pos.pieces();

    // Keep only the squares which are relatively safe
    b &= ~attackedBy[Them][PAWN] & safe;

    // Bonus for safe pawn threats on the next move
    b = pawn_attacks_bb<Us>(b) & nonPawnEnemies;
    score += ThreatByPawnPush * popcount(b);

    // Bonus for threats on the next moves against enemy queen
#ifdef CRAZYHOUSE
    if ((pos.is_house() ? pos.count<QUEEN>(Them) - pos.count_in_hand<QUEEN>(Them) : pos.count<QUEEN>(Them)) == 1)
#else
    if (pos.count<QUEEN>(Them) == 1)
#endif
    {
        bool queenImbalance = pos.count<QUEEN>() == 1;

        Square s = pos.square<QUEEN>(Them);
        safe =   mobilityArea[Us]
              & ~pos.pieces(Us, PAWN)
              & ~stronglyProtected;

        b = attackedBy[Us][KNIGHT] & attacks_bb<KNIGHT>(s);

        score += KnightOnQueen * popcount(b & safe) * (1 + queenImbalance);

        b =  (attackedBy[Us][BISHOP] & attacks_bb<BISHOP>(s, pos.pieces()))
           | (attackedBy[Us][ROOK  ] & attacks_bb<ROOK  >(s, pos.pieces()));

        score += SliderOnQueen * popcount(b & safe & attackedBy2[Us]) * (1 + queenImbalance);
    }
    }

    if constexpr (T)
        Trace::add(THREAT, Us, score);

    return score;
  }

  // Evaluation::passed() evaluates the passed pawns and candidate passed
  // pawns of the given color.

  template<Tracing T> template<Color Us>
  Score Evaluation<T>::passed() const {

    constexpr Color     Them = ~Us;
    constexpr Direction Up   = pawn_push(Us);
    constexpr Direction Down = -Up;

    auto king_proximity = [&](Color c, Square s) {
      return std::min(distance(pos.square<KING>(c), s), 5);
    };

    Bitboard b, bb, squaresToQueen, unsafeSquares, blockedPassers, helpers;
    Score score = SCORE_ZERO;

    b = pe->passed_pawns(Us);

    blockedPassers = b & shift<Down>(pos.pieces(Them, PAWN));
    if (blockedPassers)
    {
        helpers =  shift<Up>(pos.pieces(Us, PAWN))
                 & ~pos.pieces(Them)
                 & (~attackedBy2[Them] | attackedBy[Us][ALL_PIECES]);

        // Remove blocked candidate passers that don't have help to pass
        b &=  ~blockedPassers
            | shift<WEST>(helpers)
            | shift<EAST>(helpers);
    }

    while (b)
    {
        Square s = pop_lsb(b);

        assert(!(pos.pieces(Them, PAWN) & forward_file_bb(Us, s + Up)));

        int r = relative_rank(Us, s);

        Score bonus = PassedRank[pos.variant()][r];

#ifdef GRID
        if (pos.is_grid()) {} else
#endif
        if (r > RANK_3)
        {
            int w = 5 * r - 13;
            Square blockSq = s + Up;
#ifdef HORDE
            if (pos.is_horde())
            {
                // Assume a horde king distance of approximately 5
                if (pos.is_horde_color(Us))
                    bonus += make_score(0, king_proximity(Them, blockSq) * 5 * w);
                else
                    bonus += make_score(0, 15 * w);
            }
            else
#endif
#ifdef PLACEMENT
            if (pos.is_placement() && pos.count_in_hand<KING>(Us))
                bonus += make_score(0, 15 * w);
            else
#endif
#ifdef ANTI
            if (pos.is_anti()) {} else
#endif
#ifdef ATOMIC
            if (pos.is_atomic())
                bonus += make_score(0, king_proximity(Them, blockSq) * 5 * w);
            else
#endif
            {
            // Adjust bonus based on the king's proximity
            bonus += make_score(0, (  king_proximity(Them, blockSq) * 19 / 4
                                    - king_proximity(Us,   blockSq) *  2) * w);

            // If blockSq is not the queening square then consider also a second push
            if (r != RANK_7)
                bonus -= make_score(0, king_proximity(Us, blockSq + Up) * w);
            }

            // If the pawn is free to advance, then increase the bonus
            if (pos.empty(blockSq))
            {
                squaresToQueen = forward_file_bb(Us, s);
                unsafeSquares = passed_pawn_span(Us, s);

                bb = forward_file_bb(Them, s) & pos.pieces(ROOK, QUEEN);

                if (!(pos.pieces(Them) & bb))
                    unsafeSquares &= attackedBy[Them][ALL_PIECES] | pos.pieces(Them);

                // If there are no enemy pieces or attacks on passed pawn span, assign a big bonus.
                // Or if there is some, but they are all attacked by our pawns, assign a bit smaller bonus.
                // Otherwise assign a smaller bonus if the path to queen is not attacked
                // and even smaller bonus if it is attacked but block square is not.
                int k = !unsafeSquares                    ? 36 :
                !(unsafeSquares & ~attackedBy[Us][PAWN])  ? 30 :
                        !(unsafeSquares & squaresToQueen) ? 17 :
                        !(unsafeSquares & blockSq)        ?  7 :
                                                             0 ;

                // Assign a larger bonus if the block square is defended
                if ((pos.pieces(Us) & bb) || (attackedBy[Us][ALL_PIECES] & blockSq))
                    k += 5;

                bonus += make_score(k * w, k * w);
            }
        } // r > RANK_3

        score += bonus - PassedFile * edge_distance(file_of(s));
    }

    if constexpr (T)
        Trace::add(PASSED, Us, score);

    return score;
  }


  // Evaluation::space() computes a space evaluation for a given side, aiming to improve game
  // play in the opening. It is based on the number of safe squares on the four central files
  // on ranks 2 to 4. Completely safe squares behind a friendly pawn are counted twice.
  // Finally, the space bonus is multiplied by a weight which decreases according to occupancy.

  template<Tracing T> template<Color Us>
  Score Evaluation<T>::space() const {

    // Early exit if, for example, both queens or 6 minor pieces have been exchanged
    if (pos.non_pawn_material() < SpaceThreshold[pos.variant()])
        return SCORE_ZERO;

    constexpr Color Them     = ~Us;
    constexpr Direction Down = -pawn_push(Us);
    constexpr Bitboard SpaceMask =
      Us == WHITE ? CenterFiles & (Rank2BB | Rank3BB | Rank4BB)
                  : CenterFiles & (Rank7BB | Rank6BB | Rank5BB);

    // Find the available squares for our pieces inside the area defined by SpaceMask
    Bitboard safe =   SpaceMask
                   & ~pos.pieces(Us, PAWN)
                   & ~attackedBy[Them][PAWN];

    // Find all squares which are at most three squares behind some friendly pawn
    Bitboard behind = pos.pieces(Us, PAWN);
    behind |= shift<Down>(behind);
    behind |= shift<Down+Down>(behind);

    // Compute space score based on the number of safe squares and number of our pieces
    // increased with number of total blocked pawns in position.
    int bonus = popcount(safe) + popcount(behind & safe & ~attackedBy[Them][ALL_PIECES]);
    int weight = pos.count<ALL_PIECES>(Us) - 3 + std::min(pe->blocked_count(), 9);
    Score score = make_score(bonus * weight * weight / 16, 0);
#ifdef KOTH
    if (pos.is_koth())
        score += KothSafeCenter * popcount(behind & safe & Center);
#endif

    if constexpr (T)
        Trace::add(SPACE, Us, score);

    return score;
  }

  // Evaluation::variant() computes variant-specific evaluation terms.

  template<Tracing T> template<Color Us>
  Score Evaluation<T>::variant() const {

    constexpr Color Them = (Us == WHITE ? BLACK : WHITE);

    Score score = SCORE_ZERO;

#ifdef ANTI
    if (pos.is_anti())
    {
        constexpr Bitboard TRank2BB = (Us == WHITE ? Rank2BB : Rank7BB);
        bool weCapture = attackedBy[Us][ALL_PIECES] & pos.pieces(Them);
        bool theyCapture = attackedBy[Them][ALL_PIECES] & pos.pieces(Us);

        // Penalties for possible captures
        if (weCapture)
        {
            // Penalty if we only attack unprotected pieces
            bool theyDefended = attackedBy[Us][ALL_PIECES] & pos.pieces(Them) & attackedBy[Them][ALL_PIECES];
            for (PieceType pt = PAWN; pt <= KING; ++pt)
            {
                if (attackedBy[Us][pt] & pos.pieces(Them) & ~attackedBy2[Us])
                    score -= AttacksAnti[theyCapture][theyDefended][pt];
                else if (attackedBy[Us][pt] & pos.pieces(Them))
                    score -= AttacksAnti[theyCapture][theyDefended][NO_PIECE_TYPE];
            }
            // If both colors attack pieces, increase penalty with piece count
            if (theyCapture)
                score -= PieceCountAnti * pos.count<ALL_PIECES>(Us);
        }
        // Bonus if we threaten to force captures (ignoring possible discoveries)
        if (!weCapture || theyCapture)
        {
            constexpr Direction Up = pawn_push(Us);
            Bitboard b = pos.pieces(Us, PAWN);
            Bitboard pawnPushes = shift<Up>(b | (shift<Up>(b & TRank2BB) & ~pos.pieces())) & ~pos.pieces();
            Bitboard pieceMoves = (attackedBy[Us][KNIGHT] | attackedBy[Us][BISHOP] | attackedBy[Us][ROOK]
                                 | attackedBy[Us][QUEEN] | attackedBy[Us][KING]) & ~pos.pieces();
            Bitboard unprotectedPawnPushes = pawnPushes & ~attackedBy[Us][ALL_PIECES];
            Bitboard unprotectedPieceMoves = pieceMoves & ~attackedBy2[Us];

            score += ThreatsAnti[0] * popcount(attackedBy[Them][ALL_PIECES] & (pawnPushes | pieceMoves));
            score += ThreatsAnti[1] * popcount(attackedBy[Them][ALL_PIECES] & (unprotectedPawnPushes | unprotectedPieceMoves));
        }
    }
#endif
#ifdef ATOMIC
    if (pos.is_atomic())
    {
        // attackedBy may be undefined for lazy and hybrid evaluations
        // Rather than generating attackedBy (which would be complex and slow)
        // use the same (non-queen) occupancy mask for all sliding attackers
        Bitboard pieces = pos.pieces() ^ pos.pieces(QUEEN);
        for (Bitboard b = pos.pieces(Them) & ~attacks_bb<KING>(pos.square<KING>(Us)); b; )
        {
            Square s = pop_lsb(b);
            Bitboard attackers = pos.attackers_to(s, pieces) & pos.pieces(Us);
            if (! attackers)
                continue;
            Bitboard blast = (attacks_bb<KING>(s) & (pos.pieces() ^ pos.pieces(PAWN))) | s;
            int count = popcount(blast & pos.pieces(Them)) - popcount(blast & pos.pieces(Us)) - 1;
            if (blast & pos.pieces(Them, KING, QUEEN))
                count++;
            // attackedBy2 may be undefined
            // (Attacked by queen and not by 2 pieces) was inspired by "dqko"
            // since generating the full attackers set is costly and even if
            // multiple queens attack the same square, why should that matter?
            // Regardless, this is functionally equivalent and therefore cannot
            // cause a regression although attacker count is meaningless.
            if ((blast & pos.pieces(Us, QUEEN)) || (attackers == pos.pieces(Us, QUEEN) && popcount(attackers) == 1))
                count--;
            score += std::max(SCORE_ZERO, ThreatByBlast * count);
        }
        score -= AtomicConfinedKing * popcount(attacks_bb<KING>(pos.square<KING>(Us)) & pos.pieces());
    }
#endif
#ifdef HORDE
    if (pos.is_horde() && pos.is_horde_color(Them))
    {
        // Add a bonus according to how close we are to breaking through the pawn wall
        if (pos.pieces(Us, ROOK) | pos.pieces(Us, QUEEN))
        {
            int dist = 8;
            Bitboard target = (Us == WHITE ? Rank8BB : Rank1BB);
            while (target)
            {
                if (pos.attackers_to(pop_lsb(target)) & pos.pieces(Us, ROOK, QUEEN))
                    dist = 0;
            }
            for (File f = FILE_A; f <= FILE_H; ++f)
            {
                int pawns = popcount(pos.pieces(Them, PAWN) & file_bb(f));
                int pawnsl = std::min(popcount(pos.pieces(Them, PAWN) & shift<WEST>(file_bb(f))), pawns);
                int pawnsr = std::min(popcount(pos.pieces(Them, PAWN) & shift<EAST>(file_bb(f))), pawns);
                dist = std::min(dist, pawnsl + pawnsr);
            }
            score += HordeShelter * pos.count<PAWN>(Them) / (1 + dist) / (pos.pieces(Us, QUEEN) ? 2 : 4);
        }
    }
#endif
#ifdef KOTH
    if (pos.is_koth())
    {
        constexpr Direction Up = pawn_push(Us);
        Bitboard center = Center;
        while (center)
        {
            Square s = pop_lsb(center);
            int dist = distance(pos.square<KING>(Us), s)
                      + popcount(pos.attackers_to(s) & pos.pieces(Them))
                      + !!(pos.pieces(Us) & s)
                      + !!(shift<Up>(pos.pieces(Us, PAWN) & s) & pos.pieces(Them, PAWN));
            assert(dist > 0);
            score += KothDistanceBonus[std::min(dist - 1, 5)];
        }
    }
#endif
#ifdef LOSERS
    if (pos.is_losers())
    {
        constexpr Bitboard TRank2BB = (Us == WHITE ? Rank2BB : Rank7BB);
        constexpr Direction Up = pawn_push(Us);
        bool weCapture = attackedBy[Us][ALL_PIECES] & pos.pieces(Them);
        bool theyCapture = attackedBy[Them][ALL_PIECES] & pos.pieces(Us);

        // Penalties for possible captures
        if (weCapture)
        {
            // Penalty if we only attack unprotected pieces
            bool theyDefended = attackedBy[Us][ALL_PIECES] & pos.pieces(Them) & attackedBy[Them][ALL_PIECES];
            for (PieceType pt = PAWN; pt <= KING; ++pt)
            {
                if (attackedBy[Us][pt] & pos.pieces(Them) & ~attackedBy2[Us])
                    score -= AttacksLosers[theyCapture][theyDefended][pt];
                else if (attackedBy[Us][pt] & pos.pieces(Them))
                    score -= AttacksLosers[theyCapture][theyDefended][NO_PIECE_TYPE];
            }
        }
        // Bonus if we threaten to force captures (ignoring possible discoveries)
        if (!weCapture || theyCapture)
        {
            Bitboard b = pos.pieces(Us, PAWN);
            Bitboard pawnPushes = shift<Up>(b | (shift<Up>(b & TRank2BB) & ~pos.pieces())) & ~pos.pieces();
            Bitboard pieceMoves = (attackedBy[Us][KNIGHT] | attackedBy[Us][BISHOP] | attackedBy[Us][ROOK]
                                 | attackedBy[Us][QUEEN] | attackedBy[Us][KING]) & ~pos.pieces();
            Bitboard unprotectedPawnPushes = pawnPushes & ~attackedBy[Us][ALL_PIECES];
            Bitboard unprotectedPieceMoves = pieceMoves & ~attackedBy2[Us];

            score += ThreatsLosers[0] * popcount(attackedBy[Them][ALL_PIECES] & (pawnPushes | pieceMoves));
            score += ThreatsLosers[1] * popcount(attackedBy[Them][ALL_PIECES] & (unprotectedPawnPushes | unprotectedPieceMoves));
        }
    }
#endif
#ifdef THREECHECK
    if (pos.is_three_check())
        score += (popcount(pos.pieces(Us, BISHOP, KNIGHT) & WideCenter) * pos.checks_given(Us)) * pos.non_pawn_material(Us) / 16;
#endif

    if (T)
        Trace::add(VARIANT, Us, score);

    return score;
  }


  // Evaluation::winnable() adjusts the midgame and endgame score components, based on
  // the known attacking/defending status of the players. The final value is derived
  // by interpolation from the midgame and endgame values.

  template<Tracing T>
  Value Evaluation<T>::winnable(Score score) const {

    bool pawnsOnBothFlanks =   (pos.pieces(PAWN) & QueenSide)
                            && (pos.pieces(PAWN) & KingSide);

    int complexity = 0;
#ifdef ANTI
    if (pos.is_anti()) {} else
#endif
#ifdef HORDE
    if (pos.is_horde()) {} else
#endif
#ifdef PLACEMENT
    if (pos.is_placement() && (pos.count_in_hand<KING>(WHITE) || pos.count_in_hand<KING>(BLACK))) {} else
#endif
#ifdef LOSERS
    if (pos.is_losers()) {} else
#endif
    {
    int outflanking =  distance<File>(pos.square<KING>(WHITE), pos.square<KING>(BLACK))
                    + int(rank_of(pos.square<KING>(WHITE)) - rank_of(pos.square<KING>(BLACK)));

    bool almostUnwinnable =   outflanking < 0
                           && !pawnsOnBothFlanks;

    bool infiltration =   rank_of(pos.square<KING>(WHITE)) > RANK_4
                       || rank_of(pos.square<KING>(BLACK)) < RANK_5;

    // Compute the initiative bonus for the attacking side
    complexity =   9 * pe->passed_count()
                    + 12 * pos.count<PAWN>()
                    +  9 * outflanking
                    + 21 * pawnsOnBothFlanks
                    + 24 * infiltration
                    + 51 * !pos.non_pawn_material()
                    - 43 * almostUnwinnable
                    -110 ;
    }

    Value mg = mg_value(score);
    Value eg = eg_value(score);

    // Now apply the bonus: note that we find the attacking side by extracting the
    // sign of the midgame or endgame values, and that we carefully cap the bonus
    // so that the midgame and endgame scores do not change sign after the bonus.
    int u = ((mg > 0) - (mg < 0)) * std::clamp(complexity + 50, -abs(mg), 0);
    int v = ((eg > 0) - (eg < 0)) * std::max(complexity, -abs(eg));

    mg += u;
    eg += v;

    // Compute the scale factor for the winning side
    Color strongSide = eg > VALUE_DRAW ? WHITE : BLACK;
    int sf = me->scale_factor(pos, strongSide);

#ifdef ANTI
    if (pos.is_anti()) {} else
#endif
#ifdef EXTINCTION
    if (pos.is_extinction()) {} else
#endif
#ifdef PLACEMENT
    if (pos.is_placement() && pos.count_in_hand<KING>(~strongSide)) {} else
#endif
#ifdef ATOMIC
    if (pos.is_atomic())
    {
        if (   pos.non_pawn_material(~strongSide) <= RookValueMg
            && pos.count<PAWN>(WHITE) == pos.count<PAWN>(BLACK))
            sf = std::max(0, sf - pos.rule50_count() / 2);
    }
    else
#endif
#ifdef HORDE
    if (pos.is_horde() && pos.is_horde_color(~strongSide))
    {
        if (pos.non_pawn_material(~strongSide) >= QueenValueMg)
            sf = 10;
    }
    else
#endif
    // If scale factor is not already specific, scale up/down via general heuristics
    if (sf == SCALE_FACTOR_NORMAL)
    {
        if (pos.opposite_bishops())
        {
            // For pure opposite colored bishops endgames use scale factor
            // based on the number of passed pawns of the strong side.
            if (   pos.non_pawn_material(WHITE) == BishopValueMg
                && pos.non_pawn_material(BLACK) == BishopValueMg)
                sf = 18 + 4 * popcount(pe->passed_pawns(strongSide));
            // For every other opposite colored bishops endgames use scale factor
            // based on the number of all pieces of the strong side.
            else
                sf = 22 + 3 * pos.count<ALL_PIECES>(strongSide);
        }
        // For rook endgames with strong side not having overwhelming pawn number advantage
        // and its pawns being on one flank and weak side protecting its pieces with a king
        // use lower scale factor.
        else if (  pos.non_pawn_material(WHITE) == RookValueMg
                && pos.non_pawn_material(BLACK) == RookValueMg
                && pos.count<PAWN>(strongSide) - pos.count<PAWN>(~strongSide) <= 1
                && bool(KingSide & pos.pieces(strongSide, PAWN)) != bool(QueenSide & pos.pieces(strongSide, PAWN))
                && (attacks_bb<KING>(pos.square<KING>(~strongSide)) & pos.pieces(~strongSide, PAWN)))
            sf = 36;
        // For queen vs no queen endgames use scale factor
        // based on number of minors of side that doesn't have queen.
        else if (pos.count<QUEEN>() == 1)
            sf = 37 + 3 * (pos.count<QUEEN>(WHITE) == 1 ? pos.count<BISHOP>(BLACK) + pos.count<KNIGHT>(BLACK)
                                                        : pos.count<BISHOP>(WHITE) + pos.count<KNIGHT>(WHITE));
        // In every other case use scale factor based on
        // the number of pawns of the strong side reduced if pawns are on a single flank.
        else
            sf = std::min(sf, 36 + 7 * pos.count<PAWN>(strongSide)) - 4 * !pawnsOnBothFlanks;

        // Reduce scale factor in case of pawns being on a single flank
        sf -= 4 * !pawnsOnBothFlanks;
    }

    // Interpolate between the middlegame and (scaled by 'sf') endgame score
    v =  mg * int(me->game_phase())
       + eg * int(PHASE_MIDGAME - me->game_phase()) * ScaleFactor(sf) / SCALE_FACTOR_NORMAL;
    v /= PHASE_MIDGAME;

    if constexpr (T)
    {
        Trace::add(WINNABLE, make_score(u, eg * ScaleFactor(sf) / SCALE_FACTOR_NORMAL - eg_value(score)));
        Trace::add(TOTAL, make_score(mg, eg * ScaleFactor(sf) / SCALE_FACTOR_NORMAL));
    }

    return Value(v);
  }


  // Evaluation::value() is the main function of the class. It computes the various
  // parts of the evaluation and returns the value of the position from the point
  // of view of the side to move.

  template<Tracing T>
  Value Evaluation<T>::value() {

    assert(!pos.checkers());

    if (pos.is_variant_end())
        return pos.variant_result();

    // Probe the material hash table
    me = Material::probe(pos);

    // If we have a specialized evaluation function for the current material
    // configuration, call it and return.
    if (me->specialized_eval_exists())
        return me->evaluate(pos);

    // Initialize score by reading the incrementally updated scores included in
    // the position object (material + piece square tables) and the material
    // imbalance. Score is computed internally from the white point of view.
    Score score = pos.psq_score() + me->imbalance() + pos.this_thread()->trend;

    // Probe the pawn hash table
    pe = Pawns::probe(pos);
    score += pe->pawn_score(WHITE) - pe->pawn_score(BLACK);

    // Early exit if score is high
    auto lazy_skip = [&](Value lazyThreshold) {
        return abs(mg_value(score) + eg_value(score)) >   lazyThreshold
                                                        + std::abs(pos.this_thread()->bestValue) * 5 / 4
                                                        + pos.non_pawn_material() / 32;
    };

    if (lazy_skip(LazyThreshold1[pos.variant()]))
        goto make_v;

    // Main evaluation begins here
    initialize<WHITE>();
    initialize<BLACK>();

    // Pieces evaluated first (also populates attackedBy, attackedBy2).
    // Note that the order of evaluation of the terms is left unspecified.
    score +=  pieces<WHITE, KNIGHT>() - pieces<BLACK, KNIGHT>()
            + pieces<WHITE, BISHOP>() - pieces<BLACK, BISHOP>()
            + pieces<WHITE, ROOK  >() - pieces<BLACK, ROOK  >()
            + pieces<WHITE, QUEEN >() - pieces<BLACK, QUEEN >();

    score += mobility[WHITE] - mobility[BLACK];

    // More complex interactions that require fully populated attack bitboards
    score +=  king<   WHITE>() - king<   BLACK>()
            + passed< WHITE>() - passed< BLACK>();

    if (lazy_skip(LazyThreshold2))
        goto make_v;

    score +=  threats<WHITE>() - threats<BLACK>()
            + space<  WHITE>() - space<  BLACK>();

make_v:
    // Derive single value from mg and eg parts of score
    if (pos.variant() != CHESS_VARIANT)
        score += variant<WHITE>() - variant<BLACK>();
    Value v = winnable(score);

    // In case of tracing add all remaining individual evaluation terms
    if constexpr (T)
    {
        Trace::add(MATERIAL, pos.psq_score());
        Trace::add(IMBALANCE, me->imbalance());
        Trace::add(PAWN, pe->pawn_score(WHITE), pe->pawn_score(BLACK));
        Trace::add(MOBILITY, mobility[WHITE], mobility[BLACK]);
    }

    // Evaluation grain
    v = (v / 16) * 16;

    // Side to move point of view
    v = (pos.side_to_move() == WHITE ? v : -v);

    return v;
  }

  template<Tracing T>
  Value Evaluation<T>::variantValue(Value v) {
    me = Material::probe(pos);
    if (me->specialized_eval_exists())
        return me->evaluate(pos);

    Score score = variant<WHITE>() - variant<BLACK>();
    Value mg = mg_value(score), eg = eg_value(score);
    int sf = me->scale_factor(pos, eg > VALUE_DRAW ? WHITE : BLACK);
    Value v2 =  mg * int(me->game_phase())
              + eg * int(PHASE_MIDGAME - me->game_phase()) * ScaleFactor(sf) / SCALE_FACTOR_NORMAL;
    v2 /= PHASE_MIDGAME;

    return v + (pos.side_to_move() == WHITE ? v2 : -v2);
  }

  /// Fisher Random Chess: correction for cornered bishops, to fix chess960 play with NNUE

  Value fix_FRC(const Position& pos) {

    constexpr Bitboard Corners =  1ULL << SQ_A1 | 1ULL << SQ_H1 | 1ULL << SQ_A8 | 1ULL << SQ_H8;

    if (!(pos.pieces(BISHOP) & Corners))
        return VALUE_ZERO;

    int correction = 0;

    if (   pos.piece_on(SQ_A1) == W_BISHOP
        && pos.piece_on(SQ_B2) == W_PAWN)
        correction -= CorneredBishop;

    if (   pos.piece_on(SQ_H1) == W_BISHOP
        && pos.piece_on(SQ_G2) == W_PAWN)
        correction -= CorneredBishop;

    if (   pos.piece_on(SQ_A8) == B_BISHOP
        && pos.piece_on(SQ_B7) == B_PAWN)
        correction += CorneredBishop;

    if (   pos.piece_on(SQ_H8) == B_BISHOP
        && pos.piece_on(SQ_G7) == B_PAWN)
        correction += CorneredBishop;

    return pos.side_to_move() == WHITE ?  Value(3 * correction)
                                       : -Value(3 * correction);
  }

} // namespace Eval


/// evaluate() is the evaluator for the outer world. It returns a static
/// evaluation of the position from the point of view of the side to move.

Value Eval::evaluate(const Position& pos) {

  Value v;
#ifdef USE_NNUE
  bool useClassical = false;

  // Deciding between classical and NNUE eval (~10 Elo): for high PSQ imbalance we use classical,
  // but we switch to NNUE during long shuffling or with high material on the board.
  if (  !useNNUE
      || pos.variant() != CHESS_VARIANT
      || abs(eg_value(pos.psq_score())) * 5 > (849 + pos.non_pawn_material() / 64) * (5 + pos.rule50_count()))
  {
#endif
      v = Evaluation<NO_TRACE>(pos).value();          // classical
#ifdef USE_NNUE
      useClassical = abs(v) >= 298;
  }

  // If result of a classical evaluation is much lower than threshold fall back to NNUE
  if (useNNUE && !useClassical)
  {
       Value nnue     = NNUE::evaluate(pos, true);     // NNUE
       if (pos.variant() != CHESS_VARIANT)
           nnue = Evaluation<NO_TRACE>(pos).variantValue(nnue);
       int scale      = 1136 + 20 * pos.non_pawn_material() / 1024;
       Color stm      = pos.side_to_move();
       Value optimism = pos.this_thread()->optimism[stm];
       Value psq      = (stm == WHITE ? 1 : -1) * eg_value(pos.psq_score());
       int complexity = 35 * abs(nnue - psq) / 256;

       optimism = optimism * (44 + complexity) / 32;
       v = (nnue + optimism) * scale / 1024 - optimism;

       if (pos.is_chess960())
           v += fix_FRC(pos);
  }
#endif

  // Damp down the evaluation linearly when shuffling
  v = v * (208 - pos.rule50_count()) / 208;

  // Guarantee evaluation does not hit the tablebase range
  v = std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);

  return v;
}

/// trace() is like evaluate(), but instead of returning a value, it returns
/// a string (suitable for outputting to stdout) that contains the detailed
/// descriptions and values of each evaluation term. Useful for debugging.
/// Trace scores are from white's point of view

std::string Eval::trace(Position& pos) {

  if (pos.checkers())
      return "Final evaluation: none (in check)";

  std::stringstream ss;
  ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2);

  Value v;

  std::memset(scores, 0, sizeof(scores));

  // Reset any global variable used in eval
  pos.this_thread()->trend           = SCORE_ZERO;
  pos.this_thread()->bestValue       = VALUE_ZERO;
  pos.this_thread()->optimism[WHITE] = VALUE_ZERO;
  pos.this_thread()->optimism[BLACK] = VALUE_ZERO;

  v = Evaluation<TRACE>(pos).value();

  ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2)
     << " Contributing terms for the classical eval:\n"
     << "+------------+-------------+-------------+-------------+\n"
     << "|    Term    |    White    |    Black    |    Total    |\n"
     << "|            |   MG    EG  |   MG    EG  |   MG    EG  |\n"
     << "+------------+-------------+-------------+-------------+\n"
     << "|   Material | " << Term(MATERIAL)
     << "|  Imbalance | " << Term(IMBALANCE)
     << "|      Pawns | " << Term(PAWN)
     << "|    Knights | " << Term(KNIGHT)
     << "|    Bishops | " << Term(BISHOP)
     << "|      Rooks | " << Term(ROOK)
     << "|     Queens | " << Term(QUEEN)
     << "|   Mobility | " << Term(MOBILITY)
     << "|King safety | " << Term(KING)
     << "|    Threats | " << Term(THREAT)
     << "|     Passed | " << Term(PASSED)
     << "|      Space | " << Term(SPACE)
     << "|   Winnable | " << Term(WINNABLE)
     << "     Variant | " << Term(VARIANT)
     << "+------------+-------------+-------------+-------------+\n"
     << "|      Total | " << Term(TOTAL)
     << "+------------+-------------+-------------+-------------+\n";

#ifdef USE_NNUE
  if (Eval::useNNUE)
      ss << '\n' << NNUE::trace(pos) << '\n';
#endif

  ss << std::showpoint << std::showpos << std::fixed << std::setprecision(2) << std::setw(15);

  v = pos.side_to_move() == WHITE ? v : -v;
  ss << "\nClassical evaluation   " << to_cp(v) << " (white side)\n";
#ifdef USE_NNUE
  if (Eval::useNNUE)
  {
      v = NNUE::evaluate(pos, false);
      v = pos.side_to_move() == WHITE ? v : -v;
      ss << "NNUE evaluation        " << to_cp(v) << " (white side)\n";
  }
#endif

  v = evaluate(pos);
  v = pos.side_to_move() == WHITE ? v : -v;
  ss << "Final evaluation       " << to_cp(v) << " (white side)";
#ifdef USE_NNUE
  if (Eval::useNNUE)
     ss << " [with scaled NNUE, hybrid, ...]";
#endif
  ss << "\n";

  return ss.str();
}

} // namespace Stockfish
