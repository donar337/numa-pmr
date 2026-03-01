class CpuAffinity {
public:
    static void pin_to_node(int node);
    static int current_cpu();
};