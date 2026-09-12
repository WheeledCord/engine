/* The same entity as the Scheme tester in main.c, so both frontends are checked against one table. */
#include <engine>

forward tester_spawn();
forward tester_think();

public tester_spawn()
{
    think_next();
}

public tester_think()
{
    move_world(get("speed") * dt(), 0.0);
    think_next();
}

main()
{
    class_new("tester-pawn");
    class_field("tester-pawn", "speed", "float");
    class_on("tester-pawn", "spawn", "tester_spawn");
    class_on("tester-pawn", "think", "tester_think");
    class_done("tester-pawn");
}
