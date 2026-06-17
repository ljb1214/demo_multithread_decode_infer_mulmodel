#ifndef _PUSH_H
#define _PUSH_H
#include <opencv2/opencv.hpp>
#include <cstdint>

int pushToGB28181();
void stopGB28181();

int set_Mat(cv::Mat &img);
int set_Frame(uint8_t *y_data, uint8_t *u_data, uint8_t *v_data, int width, int height);
void notify_decode_finished();
#endif
