#include "sys.h"
#include "debug.h"

int main()
{
  Debug(NAMESPACE_DEBUG::init());

  Dout(dc::notice, "Entering main()...");

  std::array<char, 3> file_state{'N', 'R', 'W'};
  std::array<char, 2> file_size_state{'0', 'S'};
  std::array<char, 3> mode_state{'C', 'P', 'R'};
  std::array<char, 2> zero_init_state{'0', 'Z'};

  for (int fs = 0; fs < file_state.size(); ++fs)
  {
    for (int fss = 0; fss < file_size_state.size(); ++fss)
    {
      for (int ms = 0; ms < mode_state.size(); ++ms)
      {
        for (int zis = 0; zis < zero_init_state.size(); ++zis)
        {
          // N-R- is not possible.
          if (file_state[fs] == 'N' && mode_state[ms] == 'R')
            continue;
          // --RZ is not possibe.
          if (mode_state[ms] == 'R' && zero_init_state[zis] == 'Z')
            continue;
          // R-P- is not possible.
          if (file_state[fs] == 'R' && mode_state[ms] == 'P')
            continue;
          // N0-- is not possible.
          if (file_state[fs] == 'N' && file_size_state[fss] == '0')
            continue;
          // R--Z is not possible.
          if (file_state[fs] == 'R' && zero_init_state[zis] == 'Z')
            continue;
          // N-C- is not possible.
          if (file_state[fs] == 'N' && mode_state[ms] == 'C')
            continue;
          std::cout << file_state[fs] << file_size_state[fss] << mode_state[ms] << zero_init_state[zis] << '\n';
        }
      }
    }
  }

  Dout(dc::notice, "Leaving main()...");
}
