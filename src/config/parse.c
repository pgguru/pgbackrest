/***********************************************************************************************************************************
Command and Option Parse
***********************************************************************************************************************************/
#include "build.auto.h"

#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/debug.h"
#include "common/error.h"
#include "common/ini.h"
#include "common/log.h"
#include "common/memContext.h"
#include "common/regExp.h"
#include "config/config.intern.h"
#include "config/parse.h"
#include "storage/helper.h"
#include "version.h"

/***********************************************************************************************************************************
Define global section name
***********************************************************************************************************************************/
#define CFGDEF_SECTION_GLOBAL                                       "global"

/***********************************************************************************************************************************
Section enum - defines which sections of the config an option can appear in
***********************************************************************************************************************************/
typedef enum
{
    cfgSectionCommandLine,                                          // Command-line only
    cfgSectionGlobal,                                               // Command-line or in any config section
    cfgSectionStanza,                                               // Command-line or in any config stanza section
} ConfigSection;

/***********************************************************************************************************************************
Standard config file name and old default path and name
***********************************************************************************************************************************/
#define PGBACKREST_CONFIG_FILE                                      PROJECT_BIN ".conf"
#define PGBACKREST_CONFIG_ORIG_PATH_FILE                            "/etc/" PGBACKREST_CONFIG_FILE
    STRING_STATIC(PGBACKREST_CONFIG_ORIG_PATH_FILE_STR,             PGBACKREST_CONFIG_ORIG_PATH_FILE);

/***********************************************************************************************************************************
Prefix for environment variables
***********************************************************************************************************************************/
#define PGBACKREST_ENV                                              "PGBACKREST_"
#define PGBACKREST_ENV_SIZE                                         (sizeof(PGBACKREST_ENV) - 1)

// In some environments this will not be extern'd
extern char **environ;

/***********************************************************************************************************************************
Standard config include path name
***********************************************************************************************************************************/
#define PGBACKREST_CONFIG_INCLUDE_PATH                              "conf.d"

/***********************************************************************************************************************************
Option value constants
***********************************************************************************************************************************/
VARIANT_STRDEF_STATIC(OPTION_VALUE_0,                               ZERO_Z);
VARIANT_STRDEF_STATIC(OPTION_VALUE_1,                               ONE_Z);

/***********************************************************************************************************************************
Parse option flags
***********************************************************************************************************************************/
// Offset the option values so they don't conflict with getopt_long return codes
#define PARSE_OPTION_FLAG                                           (1 << 30)

// Add a flag for negation rather than checking "--no-"
#define PARSE_NEGATE_FLAG                                           (1 << 29)

// Add a flag for reset rather than checking "--reset-"
#define PARSE_RESET_FLAG                                            (1 << 28)

// Indicate that option name has been deprecated and will be removed in a future release
#define PARSE_DEPRECATE_FLAG                                        (1 << 27)

// Mask for option id (must be 0-255)
#define PARSE_OPTION_MASK                                           0xFF

// Shift and mask for option key index (must be 0-255)
#define PARSE_KEY_IDX_SHIFT                                         8
#define PARSE_KEY_IDX_MASK                                          0xFF

/***********************************************************************************************************************************
Define how a command is parsed
***********************************************************************************************************************************/
typedef struct ParseRuleCommand
{
    const char *name;                                               // Name
    unsigned int commandRoleValid:CFG_COMMAND_ROLE_TOTAL;           // Valid for the command role?
    bool parameterAllowed:1;                                        // Command-line parameters are allowed
} ParseRuleCommand;

// Macros used to define parse rules in parse.auto.c in a format that diffs well
#define PARSE_RULE_COMMAND(...)                                                                                                    \
    {__VA_ARGS__}

#define PARSE_RULE_COMMAND_NAME(nameParam)                                                                                         \
    .name = nameParam

#define PARSE_RULE_COMMAND_ROLE_VALID_LIST(...)                                                                                    \
    .commandRoleValid = 0 __VA_ARGS__

#define PARSE_RULE_COMMAND_ROLE(commandRoleParam)                                                                                  \
    | (1 << commandRoleParam)

#define PARSE_RULE_COMMAND_PARAMETER_ALLOWED(parameterAllowedParam)                                                                \
    .parameterAllowed = parameterAllowedParam

/***********************************************************************************************************************************
Define how an option group is parsed
***********************************************************************************************************************************/
typedef struct ParseRuleOptionGroup
{
    const char *name;                                               // All options in the group must be prefixed with this name
} ParseRuleOptionGroup;

// Macros used to define parse rules in parse.auto.c in a format that diffs well
#define PARSE_RULE_OPTION_GROUP(...)                                                                                               \
    {__VA_ARGS__}

#define PARSE_RULE_OPTION_GROUP_NAME(nameParam)                                                                                    \
    .name = nameParam

/***********************************************************************************************************************************
Define how an option is parsed and interacts with other options
***********************************************************************************************************************************/
typedef struct ParseRuleOption
{
    const char *name;                                               // Name
    unsigned int type:3;                                            // e.g. string, int, boolean
    bool required:1;                                                // Is the option required?
    unsigned int section:2;                                         // e.g. global, stanza, cmd-line
    bool secure:1;                                                  // Needs to be redacted in logs and cmd-line?
    bool multi:1;                                                   // Can be specified multiple times?
    bool group:1;                                                   // In a group?
    unsigned int groupId:1;                                         // Id if in a group
    uint32_t commandRoleValid[CFG_COMMAND_ROLE_TOTAL];              // Valid for the command role?

    const void **data;                                              // Optional data and command overrides
} ParseRuleOption;

// Define additional types of data that can be associated with an option. Because these types are rare they are not given dedicated
// fields and are instead packed into an array which is read at runtime.  This may seem inefficient but they are only accessed a
// single time during parse so space efficiency is more important than performance.
typedef enum
{
    parseRuleOptionDataTypeEnd,                                     // Indicates there is no more data
    parseRuleOptionDataTypeAllowList,
    parseRuleOptionDataTypeAllowRange,
    parseRuleOptionDataTypeCommand,
    parseRuleOptionDataTypeDefault,
    parseRuleOptionDataTypeDepend,
    parseRuleOptionDataTypeRequired,
} ParseRuleOptionDataType;

// Macros used to define parse rules in parse.auto.c in a format that diffs well
#define PARSE_RULE_OPTION(...)                                                                                                     \
    {__VA_ARGS__}

#define PARSE_RULE_OPTION_NAME(nameParam)                                                                                          \
    .name = nameParam

#define PARSE_RULE_OPTION_TYPE(typeParam)                                                                                          \
    .type = typeParam

#define PARSE_RULE_OPTION_REQUIRED(requiredParam)                                                                                  \
    .required = requiredParam

#define PARSE_RULE_OPTION_SECTION(sectionParam)                                                                                    \
    .section = sectionParam

#define PARSE_RULE_OPTION_SECURE(secureParam)                                                                                      \
    .secure = secureParam

#define PARSE_RULE_OPTION_MULTI(typeMulti)                                                                                         \
    .multi = typeMulti

#define PARSE_RULE_OPTION_GROUP_MEMBER(groupParam)                                                                                 \
    .group = groupParam

#define PARSE_RULE_OPTION_GROUP_ID(groupIdParam)                                                                                   \
    .groupId = groupIdParam

#define PARSE_RULE_OPTION_COMMAND_ROLE_DEFAULT_VALID_LIST(...)                                                                     \
    .commandRoleValid[cfgCmdRoleDefault] = 0 __VA_ARGS__

#define PARSE_RULE_OPTION_COMMAND_ROLE_ASYNC_VALID_LIST(...)                                                                       \
    .commandRoleValid[cfgCmdRoleAsync] = 0 __VA_ARGS__

#define PARSE_RULE_OPTION_COMMAND_ROLE_LOCAL_VALID_LIST(...)                                                                       \
    .commandRoleValid[cfgCmdRoleLocal] = 0 __VA_ARGS__

#define PARSE_RULE_OPTION_COMMAND_ROLE_REMOTE_VALID_LIST(...)                                                                      \
    .commandRoleValid[cfgCmdRoleRemote] = 0 __VA_ARGS__

#define PARSE_RULE_OPTION_COMMAND(commandParam)                                                                                    \
    | (1 << commandParam)

#define PARSE_RULE_OPTION_OPTIONAL_PUSH_LIST(type, size, data, ...)                                                                \
    (const void *)((uint32_t)type << 24 | (uint32_t)size << 16 | (uint32_t)data), __VA_ARGS__

#define PARSE_RULE_OPTION_OPTIONAL_LIST(...)                                                                                       \
    .data = (const void *[]){__VA_ARGS__ NULL}

#define PARSE_RULE_OPTION_OPTIONAL_PUSH(type, size, data)                                                                          \
    (const void *)((uint32_t)type << 24 | (uint32_t)size << 16 | (uint32_t)data)

#define PARSE_RULE_OPTION_OPTIONAL_COMMAND_OVERRIDE(...)                                                                           \
    __VA_ARGS__

#define PARSE_RULE_OPTION_OPTIONAL_COMMAND(command)                                                                                \
    PARSE_RULE_OPTION_OPTIONAL_PUSH(parseRuleOptionDataTypeCommand, 0, command)

#define PARSE_RULE_OPTION_OPTIONAL_ALLOW_LIST(...)                                                                                 \
    PARSE_RULE_OPTION_OPTIONAL_PUSH_LIST(                                                                                          \
        parseRuleOptionDataTypeAllowList, sizeof((const char *[]){__VA_ARGS__}) / sizeof(const char *), 0, __VA_ARGS__)

#define PARSE_RULE_OPTION_OPTIONAL_ALLOW_RANGE(rangeMinParam, rangeMaxParam)                                                       \
    PARSE_RULE_OPTION_OPTIONAL_PUSH_LIST(                                                                                          \
        parseRuleOptionDataTypeAllowRange, 4, 0,                                                                                   \
        (const void *)(intptr_t)(int32_t)((int64_t)rangeMinParam >> 32),                                                           \
        (const void *)(intptr_t)(int32_t)((int64_t)rangeMinParam & 0xFFFFFFFF),                                                    \
        (const void *)(intptr_t)(int32_t)((int64_t)rangeMaxParam >> 32),                                                           \
        (const void *)(intptr_t)(int32_t)((int64_t)rangeMaxParam & 0xFFFFFFFF))

#define PARSE_RULE_OPTION_OPTIONAL_DEFAULT(defaultParam)                                                                           \
    PARSE_RULE_OPTION_OPTIONAL_PUSH_LIST(parseRuleOptionDataTypeDefault, 1, 0, defaultParam)

#define PARSE_RULE_OPTION_OPTIONAL_DEPEND(optionDepend)                                                                            \
    PARSE_RULE_OPTION_OPTIONAL_PUSH(parseRuleOptionDataTypeDepend, 0, optionDepend)

#define PARSE_RULE_OPTION_OPTIONAL_DEPEND_LIST(optionDepend, ...)                                                                  \
    PARSE_RULE_OPTION_OPTIONAL_PUSH_LIST(                                                                                          \
        parseRuleOptionDataTypeDepend, sizeof((const char *[]){__VA_ARGS__}) / sizeof(const char *), optionDepend, __VA_ARGS__)

#define PARSE_RULE_OPTION_OPTIONAL_REQUIRED(requiredParam)                                                                         \
    PARSE_RULE_OPTION_OPTIONAL_PUSH(parseRuleOptionDataTypeRequired, 0, requiredParam)

/***********************************************************************************************************************************
Include automatically generated parse data
***********************************************************************************************************************************/
#include "config/parse.auto.c"

/***********************************************************************************************************************************
Find optional data for a command and option
***********************************************************************************************************************************/
// Extract an int64 from optional data list
#define PARSE_RULE_DATA_INT64(data, index)                                                                                         \
    ((int64_t)(intptr_t)data.list[index] << 32 | (int64_t)(intptr_t)data.list[index + 1])

// Extracted option data
typedef struct ParseRuleOptionData
{
    bool found;                                                     // Was the data found?
    int data;                                                       // Data value
    unsigned int listSize;                                          // Data list size
    const void **list;                                              // Data list
} ParseRuleOptionData;

static ParseRuleOptionData
parseRuleOptionDataFind(ParseRuleOptionDataType typeFind, ConfigCommand commandId, ConfigOption optionId)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(ENUM, typeFind);
        FUNCTION_TEST_PARAM(ENUM, commandId);
        FUNCTION_TEST_PARAM(ENUM, optionId);
    FUNCTION_TEST_END();

    ParseRuleOptionData result = {0};

    const void **dataList = parseRuleOption[optionId].data;

    // Only proceed if there is data
    if (dataList != NULL)
    {
        ParseRuleOptionDataType type;
        unsigned int offset = 0;
        unsigned int size;
        int data;
        unsigned int commandCurrent = UINT_MAX;

        // Loop through all data
        do
        {
            // Extract data
            type = (ParseRuleOptionDataType)(((uintptr_t)dataList[offset] >> 24) & 0xFF);
            size = ((uintptr_t)dataList[offset] >> 16) & 0xFF;
            data = (uintptr_t)dataList[offset] & 0xFFFF;

            // If a command block then set the current command
            if (type == parseRuleOptionDataTypeCommand)
            {
                // If data was not found in the expected command then there's nothing more to look for
                if (commandCurrent == commandId)
                    break;

                // Set the current command
                commandCurrent = (unsigned int)data;
            }
            // Only find type if not in a command block yet or in the expected command
            else if (type == typeFind && (commandCurrent == UINT_MAX || commandCurrent == commandId))
            {
                // Store the data found
                result.found = true;
                result.data = data;
                result.listSize = size;
                result.list = &dataList[offset + 1];

                // If found in the expected command block then nothing more to look for
                if (commandCurrent == commandId)
                    break;
            }

            offset += size + 1;
        }
        while (type != parseRuleOptionDataTypeEnd);
    }

    FUNCTION_TEST_RETURN(result);
}

/***********************************************************************************************************************************
Struct to hold options parsed from the command line
***********************************************************************************************************************************/
typedef struct ParseOptionValue
{
    bool found:1;                                                   // Was the option found?
    bool negate:1;                                                  // Was the option negated on the command line?
    bool reset:1;                                                   // Was the option reset on the command line?
    unsigned int source:2;                                          // Where was the option found?
    StringList *valueList;                                          // List of values found
} ParseOptionValue;

typedef struct ParseOption
{
    unsigned int indexListTotal;                                    // Total options in indexed list
    ParseOptionValue *indexList;                                    // List of indexed option values
} ParseOption;

#define FUNCTION_LOG_PARSE_OPTION_FORMAT(value, buffer, bufferSize)                                                                \
    typeToLog("ParseOption", buffer, bufferSize)

/***********************************************************************************************************************************
Get the indexed value, creating the array to contain it if needed
***********************************************************************************************************************************/
static ParseOptionValue *
parseOptionIdxValue(ParseOption *optionList, unsigned int optionId, unsigned int optionKeyIdx)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(PARSE_OPTION, parseOption);             // Structure containing all options being parsed
        FUNCTION_TEST_PARAM(UINT, optionId);                        // Unique ID which also identifies the option in the parse list
        FUNCTION_TEST_PARAM(UINT, optionKeyIdx);                    // Zero-based key index (e.g. pg3-path => 2), 0 for non-indexed
    FUNCTION_TEST_END();

    // If the requested index is beyond what has already been allocated
    if (optionKeyIdx >= optionList[optionId].indexListTotal)
    {
        // If the option is in a group
        if (parseRuleOption[optionId].group)
        {
            unsigned int optionOffset = 0;

            // Allocate enough memory to include the requested indexed or a fixed amount to avoid too many allocations
            if (optionList[optionId].indexListTotal == 0)
            {
                optionList[optionId].indexListTotal =
                    optionKeyIdx >= (LIST_INITIAL_SIZE / 2) ? optionKeyIdx + 1 : (LIST_INITIAL_SIZE / 2);
                optionList[optionId].indexList = memNew(sizeof(ParseOptionValue) * optionList[optionId].indexListTotal);
            }
            // Allocate more memory when needed. This could be more efficient but the limited number of indexes currently allowed
            // makes it difficult to get coverage on a better implementation.
            else
            {
                optionOffset = optionList[optionId].indexListTotal;
                optionList[optionId].indexListTotal = optionKeyIdx + 1;
                optionList[optionId].indexList = memResize(
                    optionList[optionId].indexList, sizeof(ParseOptionValue) * optionList[optionId].indexListTotal);
            }

            // Initialize the newly allocated memory
            for (unsigned int optKeyIdx = optionOffset; optKeyIdx < optionList[optionId].indexListTotal; optKeyIdx++)
                optionList[optionId].indexList[optKeyIdx] = (ParseOptionValue){0};
        }
        // Else the option is not in a group so there can only be one value
        else
        {
            optionList[optionId].indexList = memNew(sizeof(ParseOptionValue));
            optionList[optionId].indexListTotal = 1;
            optionList[optionId].indexList[0] = (ParseOptionValue){0};
        }
    }

    // Return the indexed value
    FUNCTION_TEST_RETURN(&optionList[optionId].indexList[optionKeyIdx]);
}

/***********************************************************************************************************************************
Find an option by name in the option list
***********************************************************************************************************************************/
// Helper to parse the option info into a structure
__attribute__((always_inline)) static inline CfgParseOptionResult
cfgParseOptionInfo(int info)
{
    return (CfgParseOptionResult)
    {
        .found = true,
        .id = info & PARSE_OPTION_MASK,
        .keyIdx = (info >> PARSE_KEY_IDX_SHIFT) & PARSE_KEY_IDX_MASK,
        .negate = info & PARSE_NEGATE_FLAG,
        .reset = info & PARSE_RESET_FLAG,
        .deprecated = info & PARSE_DEPRECATE_FLAG,
    };
}

CfgParseOptionResult
cfgParseOption(const String *optionName)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, optionName);
    FUNCTION_TEST_END();

    ASSERT(optionName != NULL);

    // Search for the option
    unsigned int findIdx = 0;

    while (optionList[findIdx].name != NULL)
    {
        if (strEqZ(optionName, optionList[findIdx].name))
            break;

        findIdx++;
    }

    // If the option was found
    if (optionList[findIdx].name != NULL)
        FUNCTION_TEST_RETURN(cfgParseOptionInfo(optionList[findIdx].val));

    FUNCTION_TEST_RETURN((CfgParseOptionResult){0});
}

/**********************************************************************************************************************************/
const char *
cfgParseOptionDefault(ConfigCommand commandId, ConfigOption optionId)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(ENUM, commandId);
        FUNCTION_TEST_PARAM(ENUM, optionId);
    FUNCTION_TEST_END();

    ASSERT(commandId < CFG_COMMAND_TOTAL);
    ASSERT(optionId < CFG_OPTION_TOTAL);

    ParseRuleOptionData data = parseRuleOptionDataFind(parseRuleOptionDataTypeDefault, commandId, optionId);

    if (data.found)
        FUNCTION_TEST_RETURN((const char *)data.list[0]);

    FUNCTION_TEST_RETURN(NULL);
}

/**********************************************************************************************************************************/
int
cfgParseOptionId(const char *optionName)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRINGZ, optionName);
    FUNCTION_TEST_END();

    ASSERT(optionName != NULL);

    int result = -1;

    for (ConfigOption optionId = 0; optionId < CFG_OPTION_TOTAL; optionId++)
        if (strcmp(optionName, parseRuleOption[optionId].name) == 0)
            result = (int)optionId;

    FUNCTION_TEST_RETURN(result);
}

/**********************************************************************************************************************************/
const char *
cfgParseOptionName(ConfigOption optionId)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(ENUM, optionId);
    FUNCTION_TEST_END();

    ASSERT(optionId < CFG_OPTION_TOTAL);

    FUNCTION_TEST_RETURN(parseRuleOption[optionId].name);
}

/**********************************************************************************************************************************/
const char *
cfgParseOptionKeyIdxName(ConfigOption optionId, unsigned int keyIdx)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(ENUM, optionId);
        FUNCTION_TEST_PARAM(UINT, keyIdx);
    FUNCTION_TEST_END();

    ASSERT(optionId < CFG_OPTION_TOTAL);
    ASSERT((!parseRuleOption[optionId].group && keyIdx == 0) || parseRuleOption[optionId].group);

    // If the option is in a group then construct the name
    if (parseRuleOption[optionId].group)
    {
        String *name = strNewFmt(
            "%s%u%s", parseRuleOptionGroup[parseRuleOption[optionId].groupId].name, keyIdx + 1,
            parseRuleOption[optionId].name + strlen(parseRuleOptionGroup[parseRuleOption[optionId].groupId].name));

        FUNCTION_TEST_RETURN(strZ(name));
    }

    // Else return the stored name
    FUNCTION_TEST_RETURN(parseRuleOption[optionId].name);
}

/**********************************************************************************************************************************/
bool
cfgParseOptionRequired(ConfigCommand commandId, ConfigOption optionId)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(ENUM, commandId);
        FUNCTION_TEST_PARAM(ENUM, optionId);
    FUNCTION_TEST_END();

    ASSERT(commandId < CFG_COMMAND_TOTAL);
    ASSERT(optionId < CFG_OPTION_TOTAL);

    ParseRuleOptionData data = parseRuleOptionDataFind(parseRuleOptionDataTypeRequired, commandId, optionId);

    if (data.found)
        FUNCTION_TEST_RETURN((bool)data.data);

    FUNCTION_TEST_RETURN(parseRuleOption[optionId].required);
}

/**********************************************************************************************************************************/
bool
cfgParseOptionSecure(ConfigOption optionId)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(ENUM, optionId);
    FUNCTION_TEST_END();

    ASSERT(optionId < CFG_OPTION_TOTAL);

    FUNCTION_TEST_RETURN(parseRuleOption[optionId].secure);
}

/**********************************************************************************************************************************/
ConfigOptionType
cfgParseOptionType(ConfigOption optionId)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(ENUM, optionId);
    FUNCTION_TEST_END();

    ASSERT(optionId < CFG_OPTION_TOTAL);

    FUNCTION_TEST_RETURN(parseRuleOption[optionId].type);
}

/**********************************************************************************************************************************/
bool
cfgParseOptionValid(ConfigCommand commandId, ConfigCommandRole commandRoleId, ConfigOption optionId)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(ENUM, commandId);
        FUNCTION_TEST_PARAM(ENUM, commandRoleId);
        FUNCTION_TEST_PARAM(ENUM, optionId);
    FUNCTION_TEST_END();

    ASSERT(commandId < CFG_COMMAND_TOTAL);
    ASSERT(optionId < CFG_OPTION_TOTAL);

    FUNCTION_TEST_RETURN(parseRuleOption[optionId].commandRoleValid[commandRoleId] & ((uint32_t)1 << commandId));
}

/***********************************************************************************************************************************
Generate multiplier based on character
***********************************************************************************************************************************/
static uint64_t
sizeQualifierToMultiplier(char qualifier)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(CHAR, qualifier);
    FUNCTION_TEST_END();

    uint64_t result;

    switch (qualifier)
    {
        case 'b':
        {
            result = 1;
            break;
        }

        case 'k':
        {
            result = 1024;
            break;
        }

        case 'm':
        {
            result = 1024 * 1024;
            break;
        }

        case 'g':
        {
            result = 1024 * 1024 * 1024;
            break;
        }

        case 't':
        {
            result = 1024LL * 1024LL * 1024LL * 1024LL;
            break;
        }

        case 'p':
        {
            result = 1024LL * 1024LL * 1024LL * 1024LL * 1024LL;
            break;
        }

        default:
            THROW_FMT(AssertError, "'%c' is not a valid size qualifier", qualifier);
    }

    FUNCTION_TEST_RETURN(result);
}

static uint64_t
convertToByte(const String *value)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, value);
    FUNCTION_TEST_END();

    ASSERT(value != NULL);

    // Lowercase the value
    String *valueLower = strLower(strDup(value));

    // Match the value against possible values
    if (regExpMatchOne(STRDEF("^[0-9]+(kb|k|mb|m|gb|g|tb|t|pb|p|b)*$"), valueLower))
    {
        // Get the character array and size
        const char *strArray = strZ(valueLower);
        size_t size = strSize(valueLower);
        int chrPos = -1;

        // If there is a 'b' on the end, then see if the previous character is a number
        if (strArray[size - 1] == 'b')
        {
            // If the previous character is a number, then the letter to look at is 'b' which is the last position else it is in the
            // next to last position (e.g. kb - so the 'k' is the position of interest).  Only need to test for <= 9 since the regex
            // enforces the format.
            if (strArray[size - 2] <= '9')
                chrPos = (int)(size - 1);
            else
                chrPos = (int)(size - 2);
        }
        // else if there is no 'b' at the end but the last position is not a number then it must be one of the letters, e.g. 'k'
        else if (strArray[size - 1] > '9')
            chrPos = (int)(size - 1);

        uint64_t multiplier = 1;

        // If a letter was found calculate multiplier, else do nothing since assumed value is already in bytes
        if (chrPos != -1)
        {
            multiplier = sizeQualifierToMultiplier(strArray[chrPos]);

            // Remove any letters
            strTrunc(valueLower, chrPos);
        }

        // Convert string to bytes
        FUNCTION_TEST_RETURN(cvtZToUInt64(strZ(valueLower)) * multiplier);
    }
    else
        THROW_FMT(FormatError, "value '%s' is not valid", strZ(value));
}

/***********************************************************************************************************************************
Load the configuration file(s)

The parent mem context is used. Defaults are passed to make testing easier.

Rules:
- config and config-include-path are default. In this case, the config file will be loaded, if it exists, and *.conf files in the
  config-include-path will be appended, if they exist. A missing/empty dir will be ignored except that the original default
  for the config file will be attempted to be loaded if the current default is not found.
- config only is specified. Only the specified config file will be loaded and is required. The default config-include-path will be
  ignored.
- config and config-path are specified. The specified config file will be loaded and is required. The overridden default of the
  config-include-path (<config-path>/conf.d) will be loaded if exists but is not required.
- config-include-path only is specified. *.conf files in the config-include-path will be loaded and the path is required to exist.
  The default config will be be loaded if it exists.
- config-include-path and config-path are specified. The *.conf files in the config-include-path will be loaded and the directory
  passed must exist. The overridden default of the config file path (<config-path>/pgbackrest.conf) will be loaded if exists but is
  not required.
- If the config and config-include-path are specified. The config file will be loaded and is expected to exist and *.conf files in
  the config-include-path will be appended and at least one is expected to exist.
- If --no-config is specified and --config-include-path is specified then only *.conf files in the config-include-path will be
  loaded; the directory is required.
- If --no-config is specified and --config-path is specified then only *.conf files in the overridden default config-include-path
  (<config-path>/conf.d) will be loaded if exist but not required.
- If --no-config is specified and neither --config-include-path nor --config-path are specified then no configs will be loaded.
- If --config-path only, the defaults for config and config-include-path will be changed to use that as a base path but the files
  will not be required to exist since this is a default override.
***********************************************************************************************************************************/
static void
cfgFileLoadPart(String **config, const Buffer *configPart)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM_P(STRING, config);
        FUNCTION_LOG_PARAM(BUFFER, configPart);
    FUNCTION_LOG_END();

    if (configPart != NULL)
    {
        String *configPartStr = strNewBuf(configPart);

        // Validate the file by parsing it as an Ini object. If the file is not properly formed, an error will occur.
        if (strSize(configPartStr) > 0)
        {
            Ini *configPartIni = iniNew();
            iniParse(configPartIni, configPartStr);

            // Create the result config file
            if (*config == NULL)
                *config = strNew("");
            // Else add an LF in case the previous file did not end with one
            else

            // Add the config part to the result config file
            strCat(*config, LF_STR);
            strCat(*config, configPartStr);
        }
    }

    FUNCTION_LOG_RETURN_VOID();
}

static String *
cfgFileLoad(                                                        // NOTE: Passing defaults to enable more complete test coverage
    const ParseOption *optionList,                                  // All options and their current settings
    const String *optConfigDefault,                                 // Current default for --config option
    const String *optConfigIncludePathDefault,                      // Current default for --config-include-path option
    const String *origConfigDefault)                                // Original --config option default (/etc/pgbackrest.conf)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM_P(PARSE_OPTION, optionList);
        FUNCTION_LOG_PARAM(STRING, optConfigDefault);
        FUNCTION_LOG_PARAM(STRING, optConfigIncludePathDefault);
        FUNCTION_LOG_PARAM(STRING, origConfigDefault);
    FUNCTION_LOG_END();

    ASSERT(optionList != NULL);
    ASSERT(optConfigDefault != NULL);
    ASSERT(optConfigIncludePathDefault != NULL);
    ASSERT(origConfigDefault != NULL);

    bool loadConfig = true;
    bool loadConfigInclude = true;

    // If the option is specified on the command line, then found will be true meaning the file is required to exist,
    // else it is optional
    bool configFound = optionList[cfgOptConfig].indexList != NULL && optionList[cfgOptConfig].indexList[0].found;
    bool configRequired = configFound;
    bool configPathRequired = optionList[cfgOptConfigPath].indexList != NULL && optionList[cfgOptConfigPath].indexList[0].found;
    bool configIncludeRequired =
        optionList[cfgOptConfigIncludePath].indexList != NULL && optionList[cfgOptConfigIncludePath].indexList[0].found;

    // Save default for later determining if must check old original default config path
    const String *optConfigDefaultCurrent = optConfigDefault;

    // If the config-path option is found on the command line, then its value will override the base path defaults for config and
    // config-include-path
    if (configPathRequired)
    {
        optConfigDefault = strNewFmt(
            "%s/%s", strZ(strLstGet(optionList[cfgOptConfigPath].indexList[0].valueList, 0)), strBaseZ(optConfigDefault));
        optConfigIncludePathDefault = strNewFmt(
            "%s/%s", strZ(strLstGet(optionList[cfgOptConfigPath].indexList[0].valueList, 0)), PGBACKREST_CONFIG_INCLUDE_PATH);
    }

    // If the --no-config option was passed then do not load the config file
    if (optionList[cfgOptConfig].indexList != NULL && optionList[cfgOptConfig].indexList[0].negate)
    {
        loadConfig = false;
        configRequired = false;
    }

    // If --config option is specified on the command line but neither the --config-include-path nor the config-path are passed,
    // then do not attempt to load the include files
    if (configFound && !(configPathRequired || configIncludeRequired))
    {
        loadConfigInclude = false;
        configIncludeRequired = false;
    }

    String *result = NULL;

    // Load the main config file
    if (loadConfig)
    {
        const String *configFileName = NULL;

        // Get the config file name from the command-line if it exists else default
        if (configRequired)
            configFileName = strLstGet(optionList[cfgOptConfig].indexList[0].valueList, 0);
        else
            configFileName = optConfigDefault;

        // Load the config file
        Buffer *buffer = storageGetP(storageNewReadP(storageLocal(), configFileName, .ignoreMissing = !configRequired));

        // Convert the contents of the file buffer to the config string object
        if (buffer != NULL)
            result = strNewBuf(buffer);
        else if (strEq(configFileName, optConfigDefaultCurrent))
        {
            // If config is current default and it was not found, attempt to load the config file from the old default location
            buffer = storageGetP(storageNewReadP(storageLocal(), origConfigDefault, .ignoreMissing = !configRequired));

            if (buffer != NULL)
                result = strNewBuf(buffer);
        }
    }

    // Load *.conf files from the include directory
    if (loadConfigInclude)
    {
        if (result != NULL)
        {
            // Validate the file by parsing it as an Ini object. If the file is not properly formed, an error will occur.
            Ini *ini = iniNew();
            iniParse(ini, result);
        }

        const String *configIncludePath = NULL;

        // Get the config include path from the command-line if it exists else default
        if (configIncludeRequired)
            configIncludePath = strLstGet(optionList[cfgOptConfigIncludePath].indexList[0].valueList, 0);
        else
            configIncludePath = optConfigIncludePathDefault;

        // Get a list of conf files from the specified path -error on missing directory if the option was passed on the command line
        StringList *list = storageListP(
            storageLocal(), configIncludePath, .expression = STRDEF(".+\\.conf$"), .errorOnMissing = configIncludeRequired,
            .nullOnMissing = !configIncludeRequired);

        // If conf files are found, then add them to the config string
        if (list != NULL && strLstSize(list) > 0)
        {
            // Sort the list for reproducibility only -- order does not matter
            strLstSort(list, sortOrderAsc);

            for (unsigned int listIdx = 0; listIdx < strLstSize(list); listIdx++)
            {
                cfgFileLoadPart(
                    &result,
                    storageGetP(
                        storageNewReadP(
                            storageLocal(), strNewFmt("%s/%s", strZ(configIncludePath), strZ(strLstGet(list, listIdx))),
                            .ignoreMissing = true)));
            }
        }
    }

    FUNCTION_LOG_RETURN(STRING, result);
}

/***********************************************************************************************************************************
??? Add validation of section names and check all sections for invalid options in the check command.  It's too expensive to add the
logic to this critical path code.
***********************************************************************************************************************************/
void
configParse(unsigned int argListSize, const char *argList[], bool resetLogLevel)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(UINT, argListSize);
        FUNCTION_LOG_PARAM(CHARPY, argList);
    FUNCTION_LOG_END();

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Create the config struct
        Config *config;

        MEM_CONTEXT_NEW_BEGIN("Config")
        {
            config = memNew(sizeof(Config));

            *config = (Config)
            {
                .memContext = MEM_CONTEXT_NEW(),
                .command = cfgCmdNone,
                .exe = strNew(argList[0]),
            };
        }
        MEM_CONTEXT_NEW_END();

        // Phase 1: parse command line parameters
        // -------------------------------------------------------------------------------------------------------------------------
        int optionValue;                                                // Value returned by getopt_long
        int optionListIdx;                                              // Index of option in list (if an option was returned)
        bool argFound = false;                                          // Track args found to decide on error or help at the end

        // Reset optind to 1 in case getopt_long has been called before
        optind = 1;

        // Don't error automatically on unknown options - they will be processed in the loop below
        opterr = false;

        // List of parsed options
        ParseOption parseOptionList[CFG_OPTION_TOTAL] = {{0}};

        // Only the first non-option parameter should be treated as a command so track if the command has been set
        bool commandSet = false;

        while ((optionValue = getopt_long((int)argListSize, (char **)argList, "-:", optionList, &optionListIdx)) != -1)
        {
            switch (optionValue)
            {
                // Parse arguments that are not options, i.e. commands and parameters passed to commands
                case 1:
                {
                    // The first argument should be the command
                    if (!commandSet)
                    {
                        const char *command = argList[optind - 1];

                        // Try getting the command from the valid command list
                        config->command = cfgCommandId(command);
                        config->commandRole = cfgCmdRoleDefault;

                        // If not successful then a command role may be appended
                        if (config->command == cfgCmdNone)
                        {
                            const StringList *commandPart = strLstNewSplit(STR(command), COLON_STR);

                            if (strLstSize(commandPart) == 2)
                            {
                                // Get command id
                                config->command = cfgCommandId(strZ(strLstGet(commandPart, 0)));

                                // If command id is valid then get command role id
                                if (config->command != cfgCmdNone)
                                    config->commandRole = cfgCommandRoleEnum(strLstGet(commandPart, 1));
                            }
                        }

                        // Error when command does not exist
                        if (config->command == cfgCmdNone)
                            THROW_FMT(CommandInvalidError, "invalid command '%s'", command);

                        // Error when role is not valid for the command
                        if (!(parseRuleCommand[config->command].commandRoleValid & ((unsigned int)1 << config->commandRole)))
                            THROW_FMT(CommandInvalidError, "invalid command/role combination '%s'", command);

                        if (config->command == cfgCmdHelp)
                            config->help = true;
                        else
                            commandSet = true;
                    }
                    // Additional arguments are command arguments
                    else
                    {
                        if (config->paramList == NULL)
                        {
                            MEM_CONTEXT_BEGIN(config->memContext)
                            {
                                config->paramList = strLstNew();
                            }
                            MEM_CONTEXT_END();
                        }

                        strLstAdd(config->paramList, strNew(argList[optind - 1]));
                    }

                    break;
                }

                // If the option is unknown then error
                case '?':
                    THROW_FMT(OptionInvalidError, "invalid option '%s'", argList[optind - 1]);

                // If the option is missing an argument then error
                case ':':
                    THROW_FMT(OptionInvalidError, "option '%s' requires argument", argList[optind - 1]);

                // Parse valid option
                default:
                {
                    // Get option id and flags from the option code
                    CfgParseOptionResult option = cfgParseOptionInfo(optionValue);

                    // Make sure the option id is valid
                    ASSERT(option.id < CFG_OPTION_TOTAL);

                    // Error if this option is secure and cannot be passed on the command line
                    if (cfgParseOptionSecure(option.id))
                    {
                        THROW_FMT(
                            OptionInvalidError,
                            "option '%s' is not allowed on the command-line\n"
                            "HINT: this option could expose secrets in the process list.\n"
                            "HINT: specify the option in a configuration file or an environment variable instead.",
                            cfgParseOptionKeyIdxName(option.id, option.keyIdx));
                    }

                    // If the option has not been found yet then set it
                    ParseOptionValue *optionValue = parseOptionIdxValue(parseOptionList, option.id, option.keyIdx);

                    if (!optionValue->found)
                    {
                        *optionValue = (ParseOptionValue)
                        {
                            .found = true,
                            .negate = option.negate,
                            .reset = option.reset,
                            .source = cfgSourceParam,
                        };

                        // Only set the argument if the option requires one
                        if (optionList[optionListIdx].has_arg == required_argument)
                        {
                            optionValue->valueList = strLstNew();
                            strLstAdd(optionValue->valueList, STR(optarg));
                        }
                    }
                    else
                    {
                        // Make sure option is not negated more than once.  It probably wouldn't hurt anything to accept this case
                        // but there's no point in allowing the user to be sloppy.
                        if (optionValue->negate && option.negate)
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' is negated multiple times",
                                cfgParseOptionKeyIdxName(option.id, option.keyIdx));
                        }

                        // Make sure option is not reset more than once.  Same justification as negate.
                        if (optionValue->reset && option.reset)
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' is reset multiple times",
                                cfgParseOptionKeyIdxName(option.id, option.keyIdx));
                        }

                        // Don't allow an option to be both negated and reset
                        if ((optionValue->reset && option.negate) || (optionValue->negate && option.reset))
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' cannot be negated and reset",
                                cfgParseOptionKeyIdxName(option.id, option.keyIdx));
                        }

                        // Don't allow an option to be both set and negated
                        if (optionValue->negate != option.negate)
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' cannot be set and negated",
                                cfgParseOptionKeyIdxName(option.id, option.keyIdx));
                        }

                        // Don't allow an option to be both set and reset
                        if (optionValue->reset != option.reset)
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' cannot be set and reset",
                                cfgParseOptionKeyIdxName(option.id, option.keyIdx));
                        }

                        // Add the argument
                        if (optionList[optionListIdx].has_arg == required_argument && parseRuleOption[option.id].multi)
                        {
                            strLstAdd(optionValue->valueList, strNew(optarg));
                        }
                        // Error if the option does not accept multiple arguments
                        else
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' cannot be set multiple times",
                                cfgParseOptionKeyIdxName(option.id, option.keyIdx));
                        }
                    }

                    break;
                }
            }

            // Arg has been found
            argFound = true;
        }

        // Handle command not found
        if (!commandSet && !config->help)
        {
            // If there are args then error
            if (argFound)
                THROW_FMT(CommandRequiredError, "no command found");

            // Otherwise set the command to help
            config->help = true;
        }

        // Error when parameters found but the command does not allow parameters
        if (config->paramList != NULL && !config->help && !parseRuleCommand[config->command].parameterAllowed)
            THROW(ParamInvalidError, "command does not allow parameters");

        // Enable logging (except for local and remote commands) so config file warnings will be output
        if (config->commandRole != cfgCmdRoleLocal && config->commandRole != cfgCmdRoleRemote && resetLogLevel)
            logInit(logLevelWarn, logLevelWarn, logLevelOff, false, 0, 1, false);

        // Only continue if command options need to be validated, i.e. a real command is running or we are getting help for a
        // specific command and would like to display actual option values in the help.
        if (config->command != cfgCmdNone && config->command != cfgCmdVersion && config->command != cfgCmdHelp)
        {
            // Phase 2: parse environment variables
            // ---------------------------------------------------------------------------------------------------------------------
            unsigned int environIdx = 0;

            // Loop through all environment variables and look for our env vars by matching the prefix
            while (environ[environIdx] != NULL)
            {
                const char *keyValue = environ[environIdx];
                environIdx++;

                if (strstr(keyValue, PGBACKREST_ENV) == keyValue)
                {
                    // Find the first = char
                    const char *equalPtr = strchr(keyValue, '=');
                    ASSERT(equalPtr != NULL);

                    // Get key and value
                    const String *key = strReplaceChr(
                        strLower(strNewN(keyValue + PGBACKREST_ENV_SIZE, (size_t)(equalPtr - (keyValue + PGBACKREST_ENV_SIZE)))),
                        '_', '-');
                    const String *value = STR(equalPtr + 1);

                    // Find the option
                    CfgParseOptionResult option = cfgParseOption(key);

                    // Warn if the option not found
                    if (!option.found)
                    {
                        LOG_WARN_FMT("environment contains invalid option '%s'", strZ(key));
                        continue;
                    }
                    // Warn if negate option found in env
                    else if (option.negate)
                    {
                        LOG_WARN_FMT("environment contains invalid negate option '%s'", strZ(key));
                        continue;
                    }
                    // Warn if reset option found in env
                    else if (option.reset)
                    {
                        LOG_WARN_FMT("environment contains invalid reset option '%s'", strZ(key));
                        continue;
                    }

                    // Continue if the option is not valid for this command
                    if (!cfgParseOptionValid(config->command, config->commandRole, option.id))
                        continue;

                    if (strSize(value) == 0)
                        THROW_FMT(OptionInvalidValueError, "environment variable '%s' must have a value", strZ(key));

                    // Continue if the option has already been specified on the command line
                    ParseOptionValue *optionValue = parseOptionIdxValue(parseOptionList, option.id, option.keyIdx);

                    if (optionValue->found)
                        continue;

                    optionValue->found = true;
                    optionValue->source = cfgSourceConfig;

                    // Convert boolean to string
                    if (cfgParseOptionType(option.id) == cfgOptTypeBoolean)
                    {
                        if (strEqZ(value, "n"))
                            optionValue->negate = true;
                        else if (!strEqZ(value, "y"))
                            THROW_FMT(OptionInvalidValueError, "environment boolean option '%s' must be 'y' or 'n'", strZ(key));
                    }
                    // Else split list/hash into separate values
                    else if (parseRuleOption[option.id].multi)
                    {
                        optionValue->valueList = strLstNewSplitZ(value, ":");
                    }
                    // Else add the string value
                    else
                    {
                        optionValue->valueList = strLstNew();
                        strLstAdd(optionValue->valueList, value);
                    }
                }
            }

            // Phase 3: parse config file unless --no-config passed
            // ---------------------------------------------------------------------------------------------------------------------
            // Load the configuration file(s)
            String *configString = cfgFileLoad(
                parseOptionList, STR(cfgParseOptionDefault(config->command, cfgOptConfig)),
                STR(cfgParseOptionDefault(config->command, cfgOptConfigIncludePath)), PGBACKREST_CONFIG_ORIG_PATH_FILE_STR);

            if (configString != NULL)
            {
                Ini *ini = iniNew();
                iniParse(ini, configString);
                // Get the stanza name
                String *stanza = NULL;

                if (parseOptionList[cfgOptStanza].indexList != NULL)
                    stanza = strLstGet(parseOptionList[cfgOptStanza].indexList[0].valueList, 0);

                // Build list of sections to search for options
                StringList *sectionList = strLstNew();

                if (stanza != NULL)
                {
                    strLstAdd(sectionList, strNewFmt("%s:%s", strZ(stanza), cfgCommandName(config->command)));
                    strLstAdd(sectionList, stanza);
                }

                strLstAdd(sectionList, strNewFmt(CFGDEF_SECTION_GLOBAL ":%s", cfgCommandName(config->command)));
                strLstAddZ(sectionList, CFGDEF_SECTION_GLOBAL);

                // Loop through sections to search for options
                for (unsigned int sectionIdx = 0; sectionIdx < strLstSize(sectionList); sectionIdx++)
                {
                    String *section = strLstGet(sectionList, sectionIdx);
                    StringList *keyList = iniSectionKeyList(ini, section);
                    KeyValue *optionFound = kvNew();

                    // Loop through keys to search for options
                    for (unsigned int keyIdx = 0; keyIdx < strLstSize(keyList); keyIdx++)
                    {
                        String *key = strLstGet(keyList, keyIdx);

                        // Find the optionName in the main list
                        CfgParseOptionResult option = cfgParseOption(key);

                        // Warn if the option not found
                        if (!option.found)
                        {
                            LOG_WARN_FMT("configuration file contains invalid option '%s'", strZ(key));
                            continue;
                        }
                        // Warn if negate option found in config
                        else if (option.negate)
                        {
                            LOG_WARN_FMT("configuration file contains negate option '%s'", strZ(key));
                            continue;
                        }
                        // Warn if reset option found in config
                        else if (option.reset)
                        {
                            LOG_WARN_FMT("configuration file contains reset option '%s'", strZ(key));
                            continue;
                        }

                        // Warn if this option should be command-line only
                        if (parseRuleOption[option.id].section == cfgSectionCommandLine)
                        {
                            LOG_WARN_FMT("configuration file contains command-line only option '%s'", strZ(key));
                            continue;
                        }

                        // Make sure this option does not appear in the same section with an alternate name
                        const Variant *optionFoundKey = VARUINT64(option.id * CFG_OPTION_KEY_MAX + option.keyIdx);
                        const Variant *optionFoundName = kvGet(optionFound, optionFoundKey);

                        if (optionFoundName != NULL)
                        {
                            THROW_FMT(
                                OptionInvalidError, "configuration file contains duplicate options ('%s', '%s') in section '[%s]'",
                                strZ(key), strZ(varStr(optionFoundName)), strZ(section));
                        }
                        else
                            kvPut(optionFound, optionFoundKey, VARSTR(key));

                        // Continue if the option is not valid for this command
                        if (!cfgParseOptionValid(config->command, config->commandRole, option.id))
                        {
                            // Warn if it is in a command section
                            if (sectionIdx % 2 == 0)
                            {
                                LOG_WARN_FMT(
                                    "configuration file contains option '%s' invalid for section '%s'", strZ(key),
                                    strZ(section));
                                continue;
                            }

                            continue;
                        }

                        // Continue if stanza option is in a global section
                        if (parseRuleOption[option.id].section == cfgSectionStanza &&
                            strBeginsWithZ(section, CFGDEF_SECTION_GLOBAL))
                        {
                            LOG_WARN_FMT(
                                "configuration file contains stanza-only option '%s' in global section '%s'", strZ(key),
                                strZ(section));
                            continue;
                        }

                        // Continue if this option has already been found in another section or command-line/environment
                        ParseOptionValue *optionValue = parseOptionIdxValue(parseOptionList, option.id, option.keyIdx);

                        if (optionValue->found)
                            continue;

                        optionValue->found = true;
                        optionValue->source = cfgSourceConfig;

                        // Process list
                        if (iniSectionKeyIsList(ini, section, key))
                        {
                            // Error if the option cannot be specified multiple times
                            if (!parseRuleOption[option.id].multi)
                            {
                                THROW_FMT(
                                    OptionInvalidError, "option '%s' cannot be set multiple times",
                                    cfgParseOptionKeyIdxName(option.id, option.keyIdx));
                            }

                            optionValue->valueList = iniGetList(ini, section, key);
                        }
                        else
                        {
                            // Get the option value
                            const String *value = iniGet(ini, section, key);

                            if (strSize(value) == 0)
                            {
                                THROW_FMT(
                                    OptionInvalidValueError, "section '%s', key '%s' must have a value", strZ(section),
                                    strZ(key));
                            }

                            if (cfgParseOptionType(option.id) == cfgOptTypeBoolean)
                            {
                                if (strEqZ(value, "n"))
                                    optionValue->negate = true;
                                else if (!strEqZ(value, "y"))
                                    THROW_FMT(OptionInvalidValueError, "boolean option '%s' must be 'y' or 'n'", strZ(key));
                            }
                            // Else add the string value
                            else
                            {
                                optionValue->valueList = strLstNew();
                                strLstAdd(optionValue->valueList, value);
                            }
                        }
                    }
                }
            }

            // Phase 4: create the config and resolve indexed options for each group
            // ---------------------------------------------------------------------------------------------------------------------
            // Determine how many indexes are used in each group
            bool groupIdxMap[CFG_OPTION_GROUP_TOTAL][CFG_OPTION_KEY_MAX] = {{0}};

            for (unsigned int optionId = 0; optionId < CFG_OPTION_TOTAL; optionId++)
            {
                // Always assign name since it may be needed for error messages
                config->option[optionId].name = parseRuleOption[optionId].name;

                // Is the option valid for this command?
                if (cfgParseOptionValid(config->command, config->commandRole, optionId))
                {
                    config->option[optionId].valid = true;
                    config->option[optionId].group = parseRuleOption[optionId].group;
                    config->option[optionId].groupId = parseRuleOption[optionId].groupId;
                }
                else
                {
                    // Error if the invalid option was explicitly set on the command-line
                    if (parseOptionList[optionId].indexList != NULL)
                    {
                        THROW_FMT(
                            OptionInvalidError, "option '%s' not valid for command '%s'", cfgParseOptionName(optionId),
                            cfgCommandName(config->command));
                    }

                    // Continue to the next option
                    continue;
                }

                // If the option is in a group
                if (parseRuleOption[optionId].group)
                {
                    unsigned int groupId = parseRuleOption[optionId].groupId;

                    config->optionGroup[groupId].valid = true;

                    // Scan the option values to determine which indexes are in use. Store them in a map that will later be scanned
                    // to create a list of just the used indexes.
                    for (unsigned int optionKeyIdx = 0; optionKeyIdx < parseOptionList[optionId].indexListTotal; optionKeyIdx++)
                    {
                        if (parseOptionList[optionId].indexList[optionKeyIdx].found &&
                            !parseOptionList[optionId].indexList[optionKeyIdx].reset)
                        {
                            if (!groupIdxMap[groupId][optionKeyIdx])
                            {
                                config->optionGroup[groupId].indexTotal++;
                                groupIdxMap[groupId][optionKeyIdx] = true;
                            }
                        }
                    }
                }
            }

            // Write the indexes into the group in order
            for (unsigned int groupId = 0; groupId < CFG_OPTION_GROUP_TOTAL; groupId++)
            {
                // Set group name
                config->optionGroup[groupId].name = parseRuleOptionGroup[groupId].name;

                // Skip the group if it is not valid
                if (!config->optionGroup[groupId].valid)
                    continue;

                // If no values were found in any index then use index 0 since all valid groups must have at least one index. This
                // may lead to an error unless all options in the group have defaults but that will be resolved later.
                if (config->optionGroup[groupId].indexTotal == 0)
                {
                    config->optionGroup[groupId].indexTotal = 1;
                }
                // Else write the key to index map for the group. This allows translation from keys to indexes and vice versa.
                else
                {
                    unsigned int optionIdxMax = 0;
                    unsigned int optionKeyIdx = 0;

                    // ??? For the pg group, key 1 is required to maintain compatibilty with older versions. Before removing this
                    // constraint the pg group remap to key 1 for remotes will need to be dealt with in the protocol/helper module.
                    if (groupId == cfgOptGrpPg)
                    {
                        optionKeyIdx = 1;
                        optionIdxMax = 1;
                    }

                    for (; optionKeyIdx < CFG_OPTION_KEY_MAX; optionKeyIdx++)
                    {
                        if (groupIdxMap[groupId][optionKeyIdx])
                        {
                            config->optionGroup[groupId].indexMap[optionIdxMax] = optionKeyIdx;
                            optionIdxMax++;
                        }
                    }
                }
            }

            // Phase 5: validate option definitions and load into configuration
            // ---------------------------------------------------------------------------------------------------------------------
            for (unsigned int optionOrderIdx = 0; optionOrderIdx < CFG_OPTION_TOTAL; optionOrderIdx++)
            {
                // Validate options based on the option resolve order.  This allows resolving all options in a single pass.
                ConfigOption optionId = optionResolveOrder[optionOrderIdx];

                // Skip this option if it is not valid
                if (!config->option[optionId].valid)
                    continue;

                // Determine the option index total. For options that are not indexed the index total is 1.
                bool optionGroup = parseRuleOption[optionId].group;
                unsigned int optionGroupId = optionGroup ? parseRuleOption[optionId].groupId : UINT_MAX;
                unsigned int optionListIndexTotal = optionGroup ? config->optionGroup[optionGroupId].indexTotal : 1;

                MEM_CONTEXT_BEGIN(config->memContext)
                {
                    config->option[optionId].index = memNew(sizeof(ConfigOptionValue) * optionListIndexTotal);
                }
                MEM_CONTEXT_END();

                // Loop through the option indexes
                ConfigOptionType optionType = cfgParseOptionType(optionId);

                for (unsigned int optionListIdx = 0; optionListIdx < optionListIndexTotal; optionListIdx++)
                {
                    // Get the key index by looking it up in the group or by defaulting to 0 for ungrouped options
                    unsigned optionKeyIdx = optionGroup ? config->optionGroup[optionGroupId].indexMap[optionListIdx] : 0;

                    // Get the parsed value using the key index. Provide a default structure when the value was not found.
                    ParseOptionValue *parseOptionValue = optionKeyIdx < parseOptionList[optionId].indexListTotal ?
                        &parseOptionList[optionId].indexList[optionKeyIdx] : &(ParseOptionValue){0};

                    // Get the location where the value will be stored in the configuration
                    ConfigOptionValue *configOptionValue = &config->option[optionId].index[optionListIdx];

                    // Is the value set for this option?
                    bool optionSet =
                        parseOptionValue->found && (optionType == cfgOptTypeBoolean || !parseOptionValue->negate) &&
                        !parseOptionValue->reset;

                    // Initialize option value and set negate and reset flag
                    *configOptionValue = (ConfigOptionValue){.negate = parseOptionValue->negate, .reset = parseOptionValue->reset};

                    // Check option dependencies
                    bool dependResolved = true;
                    ParseRuleOptionData depend = parseRuleOptionDataFind(parseRuleOptionDataTypeDepend, config->command, optionId);

                    if (depend.found)
                    {
                        ConfigOption dependOptionId = (ConfigOption)depend.data;
                        ConfigOptionType dependOptionType = cfgParseOptionType(dependOptionId);

                        ASSERT(config->option[dependOptionId].index != NULL);

                        // Get the depend option value
                        const Variant *dependValue = config->option[dependOptionId].index[optionListIdx].value;

                        if (dependValue != NULL)
                        {
                            if (dependOptionType == cfgOptTypeBoolean)
                            {
                                if (varBool(dependValue))
                                    dependValue = OPTION_VALUE_1;
                                else
                                    dependValue = OPTION_VALUE_0;
                            }
                        }

                        // Can't resolve if the depend option value is null
                        if (dependValue == NULL)
                        {
                            dependResolved = false;

                            // If depend not resolved and option value is set on the command-line then error.  See unresolved list
                            // depend below for a detailed explanation.
                            if (optionSet && parseOptionValue->source == cfgSourceParam)
                            {
                                THROW_FMT(
                                    OptionInvalidError, "option '%s' not valid without option '%s'",
                                    cfgParseOptionKeyIdxName(optionId, optionKeyIdx),
                                    cfgParseOptionKeyIdxName(dependOptionId, optionKeyIdx));
                            }
                        }
                        // If a depend list exists, make sure the value is in the list
                        else if (depend.listSize > 0)
                        {
                            dependResolved = false;

                            for (unsigned int listIdx = 0; listIdx < depend.listSize; listIdx++)
                            {
                                if (strEqZ(varStr(dependValue), (const char *)depend.list[listIdx]))
                                {
                                    dependResolved = true;
                                    break;
                                }
                            }

                            // If depend not resolved and option value is set on the command-line then error.  It's OK to have
                            // unresolved options in the config file because they may be there for another command.  For instance,
                            // spool-path is only loaded for the archive-push command when archive-async=y, and the presence of
                            // spool-path in the config file should not cause an error here, it will just end up null.
                            if (!dependResolved && optionSet && parseOptionValue->source == cfgSourceParam)
                            {
                                // Get the depend option name
                                const String *dependOptionName = STR(cfgParseOptionKeyIdxName(dependOptionId, optionKeyIdx));

                                // Build the list of possible depend values
                                StringList *dependValueList = strLstNew();

                                for (unsigned int listIdx = 0; listIdx < depend.listSize; listIdx++)
                                {
                                    const char *dependValue = (const char *)depend.list[listIdx];

                                    // Build list based on depend option type
                                    if (dependOptionType == cfgOptTypeBoolean)
                                    {
                                        // Boolean outputs depend option name as no-* when false
                                        if (strcmp(dependValue, ZERO_Z) == 0)
                                        {
                                            dependOptionName =
                                                strNewFmt("no-%s", cfgParseOptionKeyIdxName(dependOptionId, optionKeyIdx));
                                        }
                                    }
                                    else
                                    {
                                        ASSERT(dependOptionType == cfgOptTypePath || dependOptionType == cfgOptTypeString);
                                        strLstAdd(dependValueList, strNewFmt("'%s'", dependValue));
                                    }
                                }

                                // Build the error string
                                const String *errorValue = EMPTY_STR;

                                if (strLstSize(dependValueList) == 1)
                                    errorValue = strNewFmt(" = %s", strZ(strLstGet(dependValueList, 0)));
                                else if (strLstSize(dependValueList) > 1)
                                    errorValue = strNewFmt(" in (%s)", strZ(strLstJoin(dependValueList, ", ")));

                                // Throw the error
                                THROW(
                                    OptionInvalidError,
                                    strZ(
                                        strNewFmt(
                                            "option '%s' not valid without option '%s'%s",
                                            cfgParseOptionKeyIdxName(optionId, optionKeyIdx), strZ(dependOptionName),
                                            strZ(errorValue))));
                            }
                        }
                    }

                    // Is the option resolved?
                    if (dependResolved)
                    {
                        // Is the option set?
                        if (optionSet)
                        {
                            configOptionValue->source = parseOptionValue->source;

                            if (optionType == cfgOptTypeBoolean)
                            {
                                configOptionValue->value = !parseOptionValue->negate ? BOOL_TRUE_VAR : BOOL_FALSE_VAR;
                            }
                            else if (optionType == cfgOptTypeHash)
                            {
                                Variant *value = NULL;

                                MEM_CONTEXT_BEGIN(config->memContext)
                                {
                                    value = varNewKv(kvNew());
                                }
                                MEM_CONTEXT_END();

                                KeyValue *keyValue = varKv(value);

                                for (unsigned int listIdx = 0; listIdx < strLstSize(parseOptionValue->valueList); listIdx++)
                                {
                                    const char *pair = strZ(strLstGet(parseOptionValue->valueList, listIdx));
                                    const char *equal = strchr(pair, '=');

                                    if (equal == NULL)
                                    {
                                        THROW_FMT(
                                            OptionInvalidError, "key/value '%s' not valid for '%s' option",
                                            strZ(strLstGet(parseOptionValue->valueList, listIdx)),
                                            cfgParseOptionKeyIdxName(optionId, optionKeyIdx));
                                    }

                                    kvPut(keyValue, VARSTR(strNewN(pair, (size_t)(equal - pair))), VARSTRZ(equal + 1));
                                }

                                configOptionValue->value = value;
                            }
                            else if (optionType == cfgOptTypeList)
                            {
                                MEM_CONTEXT_BEGIN(config->memContext)
                                {
                                    configOptionValue->value = varNewVarLst(varLstNewStrLst(parseOptionValue->valueList));
                                }
                                MEM_CONTEXT_END();
                            }
                            else
                            {
                                String *value = strLstGet(parseOptionValue->valueList, 0);
                                const String *valueAllow = value;

                                // If a numeric type check that the value is valid
                                if (optionType == cfgOptTypeInteger ||  optionType == cfgOptTypeSize ||
                                    optionType == cfgOptTypeTime)
                                {
                                    int64_t valueInt64 = 0;

                                    // Check that the value can be converted
                                    TRY_BEGIN()
                                    {
                                        if (optionType == cfgOptTypeInteger)
                                        {
                                            MEM_CONTEXT_BEGIN(config->memContext)
                                            {
                                                configOptionValue->value = varNewInt64(cvtZToInt64(strZ(value)));
                                            }
                                            MEM_CONTEXT_END();

                                            valueInt64 = varInt64(configOptionValue->value);
                                        }
                                        else if (optionType == cfgOptTypeSize)
                                        {
                                            MEM_CONTEXT_BEGIN(config->memContext)
                                            {
                                                configOptionValue->value = varNewInt64((int64_t)convertToByte(value));
                                            }
                                            MEM_CONTEXT_END();

                                            valueInt64 = varInt64(configOptionValue->value);
                                            valueAllow = varStrForce(configOptionValue->value);
                                        }
                                        else
                                        {
                                            ASSERT(optionType == cfgOptTypeTime);

                                            MEM_CONTEXT_BEGIN(config->memContext)
                                            {
                                                configOptionValue->value = varNewInt64(
                                                    (int64_t)(cvtZToDouble(strZ(value)) * MSEC_PER_SEC));
                                            }
                                            MEM_CONTEXT_END();

                                            valueInt64 = varInt64(configOptionValue->value);
                                        }
                                    }
                                    CATCH_ANY()
                                    {
                                        THROW_FMT(
                                            OptionInvalidValueError, "'%s' is not valid for '%s' option", strZ(value),
                                            cfgParseOptionKeyIdxName(optionId, optionKeyIdx));
                                    }
                                    TRY_END();

                                    // Check value range
                                    ParseRuleOptionData allowRange = parseRuleOptionDataFind(
                                        parseRuleOptionDataTypeAllowRange, config->command, optionId);

                                    if (allowRange.found &&
                                        (valueInt64 < PARSE_RULE_DATA_INT64(allowRange, 0) ||
                                         valueInt64 > PARSE_RULE_DATA_INT64(allowRange, 2)))
                                    {
                                        THROW_FMT(
                                            OptionInvalidValueError, "'%s' is out of range for '%s' option", strZ(value),
                                            cfgParseOptionKeyIdxName(optionId, optionKeyIdx));
                                    }
                                }
                                // Else if path make sure it is valid
                                else
                                {
                                    // Make sure it is long enough to be a path
                                    if (strSize(value) == 0)
                                    {
                                        THROW_FMT(
                                            OptionInvalidValueError, "'%s' must be >= 1 character for '%s' option", strZ(value),
                                            cfgParseOptionKeyIdxName(optionId, optionKeyIdx));
                                    }

                                    if (optionType == cfgOptTypePath)
                                    {
                                        // Make sure it starts with /
                                        if (!strBeginsWithZ(value, "/"))
                                        {
                                            THROW_FMT(
                                                OptionInvalidValueError, "'%s' must begin with / for '%s' option", strZ(value),
                                                cfgParseOptionKeyIdxName(optionId, optionKeyIdx));
                                        }

                                        // Make sure there are no occurrences of //
                                        if (strstr(strZ(value), "//") != NULL)
                                        {
                                            THROW_FMT(
                                                OptionInvalidValueError, "'%s' cannot contain // for '%s' option", strZ(value),
                                                cfgParseOptionKeyIdxName(optionId, optionKeyIdx));
                                        }

                                        // If the path ends with a / we'll strip it off (unless the value is just /)
                                        if (strEndsWithZ(value, "/") && strSize(value) != 1)
                                            strTrunc(value, (int)strSize(value) - 1);
                                    }

                                    MEM_CONTEXT_BEGIN(config->memContext)
                                    {
                                        configOptionValue->value = varNewStr(value);
                                    }
                                    MEM_CONTEXT_END();
                                }

                                // If the option has an allow list then check it
                                ParseRuleOptionData allowList = parseRuleOptionDataFind(
                                    parseRuleOptionDataTypeAllowList, config->command, optionId);

                                if (allowList.found)
                                {
                                    unsigned int listIdx = 0;

                                    for (; listIdx < allowList.listSize; listIdx++)
                                    {
                                        if (strEqZ(valueAllow, (const char *)allowList.list[listIdx]))
                                            break;
                                    }

                                    if (listIdx == allowList.listSize)
                                    {
                                        THROW_FMT(
                                            OptionInvalidValueError, "'%s' is not allowed for '%s' option", strZ(value),
                                            cfgParseOptionKeyIdxName(optionId, optionKeyIdx));
                                    }
                                }
                            }
                        }
                        else if (parseOptionValue->negate)
                            configOptionValue->source = parseOptionValue->source;
                        // Else try to set a default
                        else
                        {
                            // Get the default value for this option
                            const char *value = cfgParseOptionDefault(config->command, optionId);

                            // If the option has a default
                            if (value != NULL)
                            {
                                MEM_CONTEXT_BEGIN(config->memContext)
                                {
                                    // This would typically be a switch but since not all cases are covered it would require a
                                    // separate function which does not seem worth it. The eventual plan is to have all the defaults
                                    // represented as constants so they can be assigned directly without creating variants.
                                    if (optionType == cfgOptTypeBoolean)
                                        configOptionValue->value = strcmp(value, ONE_Z) == 0 ? BOOL_TRUE_VAR : BOOL_FALSE_VAR;
                                    else if (optionType == cfgOptTypePath || optionType == cfgOptTypeString)
                                        configOptionValue->value = varNewStrZ(value);
                                    else
                                    {
                                        ASSERT(
                                            optionType == cfgOptTypeInteger || optionType == cfgOptTypeSize ||
                                            optionType == cfgOptTypeTime);

                                        configOptionValue->value = varNewInt64(cvtZToInt64(value));
                                    }
                                }
                                MEM_CONTEXT_END();
                            }
                            // Else error if option is required and help was not requested
                            else if (cfgParseOptionRequired(config->command, optionId) && !config->help)
                            {
                                const char *hint = "";

                                if (parseRuleOption[optionId].section == cfgSectionStanza)
                                    hint = "\nHINT: does this stanza exist?";

                                THROW_FMT(
                                    OptionRequiredError, "%s command requires option: %s%s", cfgCommandName(config->command),
                                    cfgParseOptionKeyIdxName(optionId, optionKeyIdx), hint);
                            }
                        }
                    }
                }
            }
        }

        // Initialize config
        cfgInit(config);

        // Set option group default index. The first index in the group is automatically set unless the group option, e.g. pg, is
        // set. For now the group default options are hard-coded but they could be dynamic. An assert has been added to make sure
        // the code breaks if a new group is added.
        for (unsigned int groupId = 0; groupId < CFG_OPTION_GROUP_TOTAL; groupId++)
        {
            ASSERT(groupId == cfgOptGrpPg || groupId == cfgOptGrpRepo);

            // Get the group default option
            unsigned int defaultOptionId = groupId == cfgOptGrpPg ? cfgOptPg : cfgOptRepo;

            // Does a default always exist?
            config->optionGroup[groupId].indexDefaultExists =
                // A default always exists for the pg group
                groupId == cfgOptGrpPg ||
                // The repo group allows a default when the repo option is valid, i.e. either repo1 is the only key set or a repo
                // is specified
                cfgOptionValid(cfgOptRepo);

            // Does the group default option exist?
            if (cfgOptionTest(defaultOptionId))
            {
                // Search for the key
                unsigned int optionKeyIdx = cfgOptionUInt(defaultOptionId) - 1;
                unsigned int index = 0;

                for (; index < cfgOptionGroupIdxTotal(groupId); index++)
                {
                    if (config->optionGroup[groupId].indexMap[index] == optionKeyIdx)
                        break;
                }

                // Error if the key was not found
                if (index == cfgOptionGroupIdxTotal(groupId))
                {
                    THROW_FMT(
                        OptionInvalidValueError, "key '%u' is not valid for '%s' option", cfgOptionUInt(defaultOptionId),
                        cfgOptionName(defaultOptionId));
                }

                // Set the default
                config->optionGroup[groupId].indexDefault = index;
                config->optionGroup[groupId].indexDefaultExists = true;
            }
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}
