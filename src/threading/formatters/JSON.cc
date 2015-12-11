// See the file "COPYING" in the main distribution directory for copyright.

#include "bro-config.h"

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <sstream>
#include <errno.h>
#include <math.h>
#include <stdint.h>

#include "./JSON.h"

using namespace threading::formatter;

JSON::JSON(MsgThread* t, TimeFormat tf, unsigned int arg_size_limit_hint) : Formatter(t), surrounding_braces(true)
	{
	timestamps = tf;
	size_limit_hint = arg_size_limit_hint;
	}

JSON::~JSON()
	{
	}

bool JSON::Describe(ODesc* desc, int num_fields, const Field* const * fields,
                    Value** vals) const
	{
	std::vector<std::string> truncated;
	std::vector<std::string>* ptruncated = 0;

	if ( surrounding_braces )
		{
		desc->AddRaw("{");
		ptruncated = &truncated;
		}

	for ( int i = 0; i < num_fields; i++ )
		{
		const u_char* bytes = desc->Bytes();
		int len = desc->Len();

		if ( i > 0 &&
		     len > 0 &&
		     bytes[len-1] != ',' &&
		     bytes[len-1] != '{' &&
		     bytes[len-1] != '[' &&
		     vals[i]->present )
			desc->AddRaw(",");

		if ( ! DescribeInternal(desc, vals[i], fields[i]->name, fields[i]->name, ptruncated) )
			return false;
		}

	if ( truncated.size() )
		{
		// Add a list of fields we had to truncate.
		desc->AddRaw(", _truncated: [");

		for ( unsigned int i = 0; i < truncated.size(); i++ )
			{
			if ( i > 0 )
				desc->AddRaw(", ");

			desc->AddRaw("\"");
			desc->AddRaw(truncated[i]);
			desc->AddRaw("\"");
			}

		desc->AddRaw("]");
		}

	if ( surrounding_braces )
		desc->AddRaw("}");

	return true;
	}

bool JSON::Describe(ODesc* desc, Value* val, const string& name) const
	{
	return DescribeInternal(desc, val, name, name, 0);
	}

bool JSON::DescribeInternal(ODesc* desc, Value* val, const string& name, const string& last_name, std::vector<std::string>* ptruncated) const
	{
	if ( ! val->present )
		return true;

	if ( name.size() )
		{
		desc->AddRaw("\"", 1);
		desc->Add(name);
		desc->AddRaw("\":", 2);
		}

	switch ( val->type )
		{
		case TYPE_BOOL:
			desc->AddRaw(val->val.int_val == 0 ? "false" : "true");
			break;

		case TYPE_INT:
			desc->Add(val->val.int_val);
			break;

		case TYPE_COUNT:
		case TYPE_COUNTER:
			{
			// JSON doesn't support unsigned 64bit ints.
			if ( val->val.uint_val >= INT64_MAX )
				{
				GetThread()->Error(GetThread()->Fmt("count value too large for JSON: %" PRIu64, val->val.uint_val));
				desc->AddRaw("null", 4);
				}
			else
				desc->Add(val->val.uint_val);
			break;
			}

		case TYPE_PORT:
			desc->Add(val->val.port_val.port);
			break;

		case TYPE_SUBNET:
			desc->AddRaw("\"", 1);
			desc->Add(Render(val->val.subnet_val));
			desc->AddRaw("\"", 1);
			break;

		case TYPE_ADDR:
			desc->AddRaw("\"", 1);
			desc->Add(Render(val->val.addr_val));
			desc->AddRaw("\"", 1);
			break;

		case TYPE_DOUBLE:
		case TYPE_INTERVAL:
			desc->Add(val->val.double_val);
			break;

		case TYPE_TIME:
			{
			if ( timestamps == TS_ISO8601 )
				{
				char buffer[40];
				char buffer2[40];
				time_t t = time_t(val->val.double_val);

				if ( strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", gmtime(&t)) > 0 )
					{
					double integ;
					double frac = modf(val->val.double_val, &integ);
					snprintf(buffer2, sizeof(buffer2), "%s.%06.0fZ", buffer, frac * 1000000);
					desc->AddRaw("\"", 1);
					desc->Add(buffer2);
					desc->AddRaw("\"", 1);
					}

				else
					GetThread()->Error(GetThread()->Fmt("strftime error for JSON: %" PRIu64));

				}

			else if ( timestamps == TS_EPOCH )
				desc->Add(val->val.double_val);

			else if ( timestamps == TS_MILLIS )
				{
				// ElasticSearch uses milliseconds for timestamps and json only
				// supports signed ints (uints can be too large).
				uint64_t ts = (uint64_t) (val->val.double_val * 1000);
				if ( ts < INT64_MAX )
					desc->Add(ts);
				else
					{
					GetThread()->Error(GetThread()->Fmt("time value too large for JSON milliseconds: %" PRIu64, ts));
					desc->AddRaw("null", 4);
					}
				}

			break;
			}

		case TYPE_ENUM:
		case TYPE_STRING:
		case TYPE_FILE:
		case TYPE_FUNC:
			{
			desc->AddRaw("\"", 1);

			unsigned int orig_len = desc->Len();

			for ( int i = 0; i < val->val.string_val.length; ++i )
				{
				char c = val->val.string_val.data[i];

				// 2byte Unicode escape special characters.
				if ( c < 32 || c > 126 || c == '\n' || c == '"' || c == '\'' || c == '\\' || c == '&' )
					{
					desc->AddRaw("\\u00", 4);
					char hex[2] = {'0', '0'};
					bytetohex(c, hex);
					desc->AddRaw(hex, 1);
					desc->AddRaw(hex + 1, 1);
					}
				else
					desc->AddRaw(&c, 1);
				}

			if ( size_limit_hint && (unsigned int)desc->Len() > size_limit_hint )
				{
				// Cut off and add "truncated" note instead.
				int delta = (desc->Len() - orig_len);
				int keep = (size_limit_hint / 100);

				if ( delta > keep )
					{
					desc->Truncate(orig_len + keep);
					desc->Add("<truncated ");
					desc->Add(delta - keep);
					desc->Add(" bytes>");

					ptruncated->push_back(last_name.size() ? last_name : "???");
					}
				}

			desc->AddRaw("\"", 1);
			break;
			}

		case TYPE_TABLE:
			{
			desc->AddRaw("[", 1);

			for ( int j = 0; j < val->val.set_val.size; j++ )
				{
				if ( j > 0 )
					desc->AddRaw(",", 1);

				DescribeInternal(desc, val->val.set_val.vals[j], "", name, ptruncated);

				if ( size_limit_hint && (unsigned int)desc->Len() > size_limit_hint )
					{
					ptruncated->push_back(last_name.size() ? last_name : "???");
					break;
					}
				}

			desc->AddRaw("]", 1);
			break;
			}

		case TYPE_VECTOR:
			{
			desc->AddRaw("[", 1);

			for ( int j = 0; j < val->val.vector_val.size; j++ )
				{
				if ( j > 0 )
					desc->AddRaw(",", 1);

				DescribeInternal(desc, val->val.vector_val.vals[j], "", name, ptruncated);

				if ( size_limit_hint && (unsigned int)desc->Len() > size_limit_hint )
					{
					ptruncated->push_back(last_name.size() ? last_name : "???");
					break;
					}
				}

			desc->AddRaw("]", 1);
			break;
			}

		default:
			return false;
		}

	return true;
	}

threading::Value* JSON::ParseValue(const string& s, const string& name, TypeTag type, TypeTag subtype) const
	{
	GetThread()->Error("JSON formatter does not support parsing yet.");
	return NULL;
	}

void JSON::SurroundingBraces(bool use_braces)
	{
	surrounding_braces = use_braces;
	}
