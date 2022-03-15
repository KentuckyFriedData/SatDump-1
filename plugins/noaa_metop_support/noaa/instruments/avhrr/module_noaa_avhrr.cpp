#include "module_noaa_avhrr.h"
#include <fstream>
#include "avhrr_reader.h"
#include "logger.h"
#include <filesystem>
#include "imgui/imgui.h"
#include "../../noaa.h"
#include "common/utils.h"
#include "common/repack.h"
#include "nlohmann/json_utils.h"

#define BUFFER_SIZE 8192

// Return filesize
size_t getFilesize(std::string filepath);

namespace noaa
{
    namespace avhrr
    {
        NOAAAVHRRDecoderModule::NOAAAVHRRDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
            : ProcessingModule(input_file, output_file_hint, parameters),
              gac_mode(parameters.count("gac_mode") > 0 ? parameters["gac_mode"].get<bool>() : 0)
        {
        }

        void NOAAAVHRRDecoderModule::process()
        {
            filesize = getFilesize(d_input_file);
            std::ifstream data_in(d_input_file, std::ios::binary);

            std::string directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/AVHRR";

            logger->info("Using input frames " + d_input_file);
            logger->info("Decoding to " + directory);

            time_t lastTime = 0;

            AVHRRReader reader(gac_mode);

            uint16_t buffer[11090];

            logger->info("Demultiplexing and deframing...");

            std::vector<double> timestamps;

            time_t dayYearValue = 0; // Start of year

            {
                time_t curr_time = time(NULL);
                struct tm timeinfo_struct;
#ifdef _WIN32
                memcpy(&timeinfo_struct, gmtime(&curr_time), sizeof(struct tm));
#else
                gmtime_r(&curr_time, &timeinfo_struct);
#endif

                // Reset to be year
                timeinfo_struct.tm_mday = 0;
                timeinfo_struct.tm_wday = 0;
                timeinfo_struct.tm_yday = 0;
                timeinfo_struct.tm_mon = 0;

                dayYearValue = mktime(&timeinfo_struct);
                dayYearValue = dayYearValue - (dayYearValue % 86400);
            }

            std::vector<int> spacecraft_ids;

            uint8_t buffer_gac[4159];

            // double last_timestamp = 0;

            while (!data_in.eof())
            {
                if (gac_mode)
                {
                    // Read and repack to 10-bits
                    data_in.read((char *)buffer_gac, 4159);
                    repackBytesTo10bits(buffer_gac, 4159, buffer);
                }
                else
                {
                    // Read buffer
                    data_in.read((char *)buffer, 11090 * 2);
                }

                reader.work(buffer);

                // Parse timestamp
                int day_of_year = buffer[8] >> 1;
                uint64_t milliseconds = (buffer[9] & 0x7F) << 20 | (buffer[10] << 10) | buffer[11];
                double timestamp = dayYearValue + (day_of_year * 86400) + double(milliseconds) / 1000.0;

                // if (last_timestamp < timestamp)
                timestamps.push_back(timestamp);
                // else
                //     timestamps.push_back(-1);

                // last_timestamp = timestamp;

                // Parse ID
                spacecraft_ids.push_back(((buffer[6] & 0x078) >> 3) & 0x000F);

                progress = data_in.tellg();

                if (time(NULL) % 10 == 0 && lastTime != time(NULL))
                {
                    lastTime = time(NULL);
                    logger->info("Progress " + std::to_string(round(((float)progress / (float)filesize) * 1000.0f) / 10.0f) + "%");
                }
            }

            /*{
                double cnt = 0;
                for (double tt : timestamps)
                    if (tt != -1)
                        cnt++;

                double avg = 0;
                double v = 1.0 / cnt;

                for (double tt : timestamps)
                    avg += tt * v;

                for (double &tt : timestamps)
                {
                    if (std::abs(tt - avg) > 3600 * 60 * 4 && tt != -1)
                    {
                        tt = -1;
                    }
                }
            }*/

            data_in.close();

            logger->info("AVHRR Lines            : " + std::to_string(reader.lines));

            logger->info("Writing images.... (Can take a while)");

            if (!std::filesystem::exists(directory))
                std::filesystem::create_directory(directory);

            image::Image<uint16_t> image1 = reader.getChannel(0);
            image::Image<uint16_t> image2 = reader.getChannel(1);
            image::Image<uint16_t> image3 = reader.getChannel(2);
            image::Image<uint16_t> image4 = reader.getChannel(3);
            image::Image<uint16_t> image5 = reader.getChannel(4);

            logger->info("Channel 1...");
            WRITE_IMAGE(image1, directory + "/AVHRR-1.png");

            logger->info("Channel 2...");
            WRITE_IMAGE(image2, directory + "/AVHRR-2.png");

            logger->info("Channel 3...");
            WRITE_IMAGE(image3, directory + "/AVHRR-3.png");

            logger->info("Channel 4...");
            WRITE_IMAGE(image4, directory + "/AVHRR-4.png");

            logger->info("Channel 5...");
            WRITE_IMAGE(image5, directory + "/AVHRR-5.png");

            // Reproject to an equirectangular proj
            if (reader.lines > 0)
            {
                // nlohmann::json satData = loadJsonFile(d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/sat_info.json");
                int norad = 0; // 28654; //satData.contains("norad") > 0 ? satData["norad"].get<int>() : 0;
                // image4.equalize();

                // Identify satellite, and apply per-sat settings...
                nlohmann::json jData;
                int scid = most_common(spacecraft_ids.begin(), spacecraft_ids.end());
                if (scid == 7) // N15
                {
                    norad = 25338;
                    logger->info("Identified NOAA-15!");

                    jData["scid"] = scid;
                    jData["name"] = "NOAA-15";
                    jData["norad"] = norad;
                }
                else if (scid == 13) // N18
                {
                    norad = 28654;
                    logger->info("Identified NOAA-18!");
                    jData["scid"] = scid;
                    jData["name"] = "NOAA-15";
                    jData["norad"] = norad;
                }
                else if (scid == 15) // N19
                {
                    norad = 33591;
                    logger->info("Identified NOAA-19!");

                    jData["scid"] = scid;
                    jData["name"] = "NOAA-15";
                    jData["norad"] = norad;
                }
                else
                {
                    logger->error("Unknwon NOAA Satellite! Only the KLM series 15, 18 and 19 are supported as others were decomissioned!");
                    jData["scid"] = scid;
                    jData["name"] = "NOAA-UNKNOWN";
                    jData["norad"] = norad;
                }

                // For later decoders
                saveJsonFile(d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/sat_info.json", jData);
            }
        }

        void NOAAAVHRRDecoderModule::drawUI(bool window)
        {
            ImGui::Begin("NOAA AVHRR Decoder", NULL, window ? NULL : NOWINDOW_FLAGS);

            ImGui::ProgressBar((float)progress / (float)filesize, ImVec2(ImGui::GetWindowWidth() - 10, 20 * ui_scale));

            ImGui::End();
        }

        std::string NOAAAVHRRDecoderModule::getID()
        {
            return "noaa_avhrr";
        }

        std::vector<std::string> NOAAAVHRRDecoderModule::getParameters()
        {
            return {};
        }

        std::shared_ptr<ProcessingModule> NOAAAVHRRDecoderModule::getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters)
        {
            return std::make_shared<NOAAAVHRRDecoderModule>(input_file, output_file_hint, parameters);
        }
    } // namespace avhrr
} // namespace noaa