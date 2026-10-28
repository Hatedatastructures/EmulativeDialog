// Boost.JSON must instantiate its compiled implementation in exactly one
// translation unit. EmulativeDialogBoostJson owns that boundary so provider
// adapters can each include <boost/json.hpp> without ODR conflicts.
#include <boost/json/src.hpp>
