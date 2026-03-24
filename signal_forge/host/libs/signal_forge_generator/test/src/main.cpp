#include <iostream>
#include <fstream>

#include "gtest/gtest.h"

#include "signal_forge_generator/generator.h"
#include "signal_forge_generator/header_parser.h"

TEST(signal_forge_generator_test, parse_header) {
    parse_header("C:/Users/LOAR02/Source/signal/signal_forge/host/libs/nodes/my_block/my_block.h");
}

TEST(signal_forge_generator_test, generate_graph) {
    signal_forge::BlockTemplate asdasd;
    asdasd.type = "my_block";
    asdasd.is_function_block = true;
    asdasd.input_templates.push_back(signal_forge::Pin {
        .direction = signal_forge::PinDirection::INPUT,
        .name = "input1"
    });
    asdasd.input_templates.push_back(signal_forge::Pin {
        .direction = signal_forge::PinDirection::INPUT,
        .name = "input2"});
    asdasd.input_templates.push_back(signal_forge::Pin {
        .direction = signal_forge::PinDirection::INPUT,
        .name = "input3"});
    asdasd.output_templates.push_back(signal_forge::Pin {
        .direction = signal_forge::PinDirection::OUTPUT,
        .name = "output1"
    });
    asdasd.output_templates.push_back(signal_forge::Pin {
        .direction = signal_forge::PinDirection::OUTPUT,
        .name = "output1"
    });

    signal_forge::Graph graph;
    graph.RegisterBlockTemplate(asdasd);
    graph.AddNode("my_block", 10.0, 10.0);
    graph.AddNode("my_block", 20.0, 20.0);

    graph.AddLink(11, 2);
    graph.AddLink(12, 3);
    //graph.AddLink(6, 10);

    signal_forge::Generator gen(graph);
    signal_forge::Generator::Result res = gen.generate();
    std::ofstream outfile("output.c");
    outfile << res.c_source;
    outfile.close();
}