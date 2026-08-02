#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectSchemaVersion.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/LocalesUtils.hpp"

#include <algorithm>

#include <cereal/types/polymorphic.hpp>
#include <cereal/types/string.hpp> 
#include <cereal/types/vector.hpp> 
#include <cereal/archives/binary.hpp>

using namespace Slic3r;

TEST_CASE("Project schema registry handles versions", "[Config][FlowVariant]")
{
    const auto& schema = ProjectSchemaRegistry::definition();
    REQUIRE(schema.config_key == std::string("project_schema_version"));
    REQUIRE(schema.current_version == 1);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.erase(schema.config_key);
    REQUIRE(ProjectSchemaRegistry::version_from(config) == schema.legacy_version);
    REQUIRE_FALSE(ProjectSchemaRegistry::is_newer(config));

    config.set_key_value(schema.config_key, new ConfigOptionInt(schema.current_version));
    REQUIRE_FALSE(ProjectSchemaRegistry::is_newer(config));

    config.set_key_value(schema.config_key, new ConfigOptionInt(schema.current_version + 1));
    REQUIRE(ProjectSchemaRegistry::is_newer(config));
}

SCENARIO("Generic config validation performs as expected.", "[Config]") {
    GIVEN("A config generated from default options") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        WHEN( "line_width is set to 250%, a valid value") {
            config.set_deserialize_strict("line_width", "250%");
            THEN( "The config is read as valid.") {
                REQUIRE(config.validate().empty());
            }
        }
        WHEN( "line_width is set to -10, an invalid value") {
            config.set("line_width", -10);
            THEN( "Validate returns error") {
                REQUIRE(! config.validate().empty());
            }
        }

        WHEN( "wall_loops is set to -10, an invalid value") {
            config.set("wall_loops", -10);
            THEN( "Validate returns error") {
                REQUIRE(! config.validate().empty());
            }
        }
    }
}

SCENARIO("Config accessor functions perform as expected.", "[Config]") {
    GIVEN("A config generated from default options") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        WHEN("A boolean option is set to a boolean value") {
            REQUIRE_NOTHROW(config.set("gcode_comments", true));
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == true);
            }
        }
        WHEN("A boolean option is set to a string value representing a 0 or 1") {
            CHECK_NOTHROW(config.set_deserialize_strict("gcode_comments", "1"));
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == true);
            }
        }
        WHEN("A boolean option is set to a string value representing something other than 0 or 1") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("gcode_comments", "Z"), BadOptionTypeException);
            }
            AND_THEN("Value is unchanged.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == false);
            }
        }
        WHEN("A boolean option is set to an int value") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("gcode_comments", 1), BadOptionTypeException);
            }
        }
        WHEN("A numeric option is set from serialized string") {
            config.set_deserialize_strict("nozzle_diameter", "0.6");
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloats>("nozzle_diameter")->get_at(0) == 0.6);
            }
        }
#if 0
		//FIXME better design accessors for vector elements.
		WHEN("An integer-based option is set through the integer interface") {
            config.set("bed_temperature", 100);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionInts>("bed_temperature")->get_at(0) == 100);
            }
        }
#endif
        WHEN("An floating-point option is set through the integer interface") {
            config.set("bridge_flow", 10);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloat>("bridge_flow")->getFloat() == 10.0);
            }
        }
        WHEN("A floating-point option is set through the double interface") {
            config.set("bridge_flow", 5.5);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloat>("bridge_flow")->getFloat() == 5.5);
            }
        }
        WHEN("An integer-based option is set through the double interface") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("wall_loops", 5.5), BadOptionTypeException);
            }
        }
        WHEN("A numeric option is set to a non-numeric value.") {
            THEN("A BadOptionTypeException exception is thown.") {
                REQUIRE_THROWS_AS(config.set_deserialize_strict("bridge_flow", "zzzz"), BadOptionValueException);
            }
            THEN("The value does not change.") {
                REQUIRE(config.opt<ConfigOptionFloat>("bridge_flow")->getFloat() == 1.0);
            }
        }
        WHEN("A string option is set through the string interface") {
            config.set("machine_end_gcode", "100");
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == "100");
            }
        }
        WHEN("A string option is set through the integer interface") {
            config.set("machine_end_gcode", 100);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == "100");
            }
        }
        WHEN("A string option is set through the double interface") {
            config.set("machine_end_gcode", 100.5);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == float_to_string_decimal_point(100.5));
            }
        }
        WHEN("A float or percent is set as a percent through the string interface.") {
            config.set_deserialize_strict("initial_layer_line_width", "100%");
            THEN("Value and percent flag are 100/true") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == true);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the string interface.") {
            config.set_deserialize_strict("initial_layer_line_width", "100");
            THEN("Value and percent flag are 100/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the int interface.") {
            config.set("initial_layer_line_width", 100);
            THEN("Value and percent flag are 100/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the double interface.") {
            config.set("initial_layer_line_width", 100.5);
            THEN("Value and percent flag are 100.5/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100.5);
            }
        }
        WHEN("An invalid option is requested during set.") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", 1), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", 1.0), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", "1"), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", true), UnknownOptionException);
            }
        }

        WHEN("An invalid option is requested during get.") {
            THEN("A UnknownOptionException exception is thrown.") {
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionString>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionFloat>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionInt>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionBool>("deadbeef_invalid_option", false), UnknownOptionException);
            }
        }
        WHEN("An invalid option is requested during opt.") {
            THEN("A UnknownOptionException exception is thrown.") {
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionString>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionFloat>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionInt>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionBool>("deadbeef_invalid_option", false), UnknownOptionException);
            }
        }

        WHEN("getX called on an unset option.") {
            THEN("The default is returned.") {
                REQUIRE(config.opt_float("layer_height") == 0.2);
                REQUIRE(config.opt_int("raft_layers") == 0);
                REQUIRE(config.opt_bool("enable_support") == false);
            }
        }

        WHEN("getFloat called on an option that has been set.") {
            config.set("layer_height", 0.5);
            THEN("The set value is returned.") {
                REQUIRE(config.opt_float("layer_height") == 0.5);
            }
        }
    }
}

SCENARIO("Config ini load/save interface", "[Config]") {
    WHEN("new_from_ini is called") {
		Slic3r::DynamicPrintConfig config;
		std::string path = std::string(TEST_DATA_DIR) + "/test_config/new_from_ini.ini";
		config.load_from_ini(path, ForwardCompatibilitySubstitutionRule::Disable);
        THEN("Config object contains ini file options.") {
			REQUIRE(config.option_throw<ConfigOptionStrings>("filament_colour", false)->values.size() == 1);
			REQUIRE(config.option_throw<ConfigOptionStrings>("filament_colour", false)->values.front() == "#ABCD");
        }
    }
}

SCENARIO("DynamicPrintConfig serialization", "[Config]") {
    WHEN("DynamicPrintConfig is serialized and deserialized") {
        FullPrintConfig full_print_config;
        DynamicPrintConfig cfg;
        cfg.apply(full_print_config, false);

        std::string serialized;
        try {
            std::ostringstream ss;
            cereal::BinaryOutputArchive oarchive(ss);
            oarchive(cfg);
            serialized = ss.str();
        } catch (const std::runtime_error & /* e */) {
            // e.what();
        }

        THEN("Config object contains ini file options.") {
            DynamicPrintConfig cfg2;
            try {
                std::stringstream ss(serialized);
                cereal::BinaryInputArchive iarchive(ss);
                iarchive(cfg2);
            } catch (const std::runtime_error & /* e */) {
                // e.what();
            }
            REQUIRE(cfg == cfg2);
        }
    }
}

TEST_CASE("DynamicPrintConfig normalizes support filament types from filament_ids", "[Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionStrings>("filament_type", true)->values      = { "PLA", "PA" };
    config.option<ConfigOptionStrings>("filament_ids", true)->values       = { "GFS00", "GFS01" };
    config.option<ConfigOptionBools>("filament_is_support", true)->values  = { true, true };

    std::string display_type;
    CHECK(config.get_filament_type(display_type, 0) == "PLA-S");
    CHECK(display_type == "Sup.PLA");

    CHECK(config.get_filament_type(display_type, 1) == "PA-S");
    CHECK(display_type == "Sup.PA");
}

TEST_CASE("DynamicPrintConfig keeps ordinary filament types unchanged", "[Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionStrings>("filament_type", true)->values      = { "PLA" };
    config.option<ConfigOptionStrings>("filament_ids", true)->values       = { "GFSL99" };
    config.option<ConfigOptionBools>("filament_is_support", true)->values  = { false };

    std::string display_type;
    CHECK(config.get_filament_type(display_type, 0) == "PLA");
    CHECK(display_type == "PLA");
}

SCENARIO("Out of range values coming from a foreign 3mf are repaired on import.", "[Config]") {
    GIVEN("A config generated from default options") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        // Precondition: the defaults themselves must be clean, otherwise the assertions below are meaningless.
        REQUIRE(config.validate().empty());

        WHEN("It carries the values a MakerWorld 3mf is known to store") {
            // These are exactly the keys and values reported by the "Invalid values found in the 3mf" notification.
            config.set("raft_first_layer_expansion", -1.0);
            config.set("solid_infill_filament", 0);
            config.set("sparse_infill_filament", 0);
            config.set("tree_support_wall_count", -1);
            config.set("wall_filament", 0);

            std::map<std::string, std::string> repaired = config.repair_out_of_range_values();

            THEN("Every offending key is reported as repaired") {
                // Snapmaker: raft_first_layer_expansion accepts -1 ("auto", min = -1) on this branch,
                // so unlike the Nokoff fork it is in range and must be left alone.
                REQUIRE(repaired.size() == 4);
                REQUIRE(repaired.count("raft_first_layer_expansion") == 0);
                REQUIRE(repaired.count("solid_infill_filament") == 1);
                REQUIRE(repaired.count("sparse_infill_filament") == 1);
                REQUIRE(repaired.count("tree_support_wall_count") == 1);
                REQUIRE(repaired.count("wall_filament") == 1);
            }
            THEN("Each value is clamped into the supported range") {
                // Filament indices are 1 based here, so the "inherit" sentinel becomes the first filament.
                REQUIRE(config.opt_int("solid_infill_filament") == 1);
                REQUIRE(config.opt_int("sparse_infill_filament") == 1);
                REQUIRE(config.opt_int("wall_filament") == 1);
                // 0 support wall loops means auto, which is what -1 means in the source slicer.
                REQUIRE(config.opt_int("tree_support_wall_count") == 0);
                REQUIRE_THAT(config.opt_float("raft_first_layer_expansion"), Catch::Matchers::WithinAbs(-1.0, 1e-9));
            }
            THEN("The repaired config passes validation, so no error is raised to the user") {
                REQUIRE(config.validate().empty());
            }
            THEN("The report describes the change that was made") {
                REQUIRE(repaired["wall_filament"] == "0 -> 1");
                REQUIRE(repaired["tree_support_wall_count"] == "-1 -> 0");
            }
        }

        WHEN("Every value is already within range") {
            THEN("Nothing is touched") {
                REQUIRE(config.repair_out_of_range_values().empty());
                REQUIRE(config.validate().empty());
            }
        }

        WHEN("A nullable vector option holds both a nil marker and an out of range value") {
            // filament_z_hop inherits <0, 5> from z_hop and is nullable.
            auto *opt = config.option<ConfigOptionFloatsNullable>("filament_z_hop", true);
            opt->values = { ConfigOptionFloatsNullable::nil_value(), 9.0 };

            std::map<std::string, std::string> repaired = config.repair_out_of_range_values();

            THEN("The nil marker survives and only the real value is clamped") {
                REQUIRE(repaired.count("filament_z_hop") == 1);
                REQUIRE(opt->is_nil(0));
                REQUIRE_THAT(opt->values[1], Catch::Matchers::WithinAbs(5.0, 1e-9));
            }
        }
    }
}

SCENARIO("Foreign sentinel values are translated while a config is deserialized.", "[Config]") {
    GIVEN("A config generated from default options") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();

        WHEN("A filament index arrives as the \"inherit\" sentinel 0") {
            config.set_deserialize_strict("wall_filament", "0");
            config.set_deserialize_strict("sparse_infill_filament", "0");
            config.set_deserialize_strict("solid_infill_filament", "0");

            THEN("It is mapped onto the first filament") {
                REQUIRE(config.opt_int("wall_filament") == 1);
                REQUIRE(config.opt_int("sparse_infill_filament") == 1);
                REQUIRE(config.opt_int("solid_infill_filament") == 1);
            }
        }

        WHEN("Support wall loops arrive as the \"auto\" sentinel -1") {
            config.set_deserialize_strict("tree_support_wall_count", "-1");

            THEN("It is mapped onto 0, which spells auto here") {
                REQUIRE(config.opt_int("tree_support_wall_count") == 0);
            }
        }

        WHEN("support_filament arrives as 0") {
            config.set_deserialize_strict("support_filament", "0");

            THEN("It is left alone, because 0 is a valid value for that key") {
                REQUIRE(config.opt_int("support_filament") == 0);
            }
        }
    }
}
