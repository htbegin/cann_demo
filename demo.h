#ifndef A2_ROCE_DEMO_H
#define A2_ROCE_DEMO_H
enum source_memory { SOURCE_HBM = 0, SOURCE_HOST = 1 };
int run_demo(int argc, char **argv, enum source_memory memory);
int run_bandwidth(int argc, char **argv, enum source_memory memory);
#endif
