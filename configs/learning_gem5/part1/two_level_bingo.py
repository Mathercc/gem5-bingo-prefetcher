# import the m5 (gem5) library created when gem5 is built
import m5
import os
import shlex

from m5.objects import (
    System, Root, Process, SEWorkload,
    SrcClockDomain, VoltageDomain,
    AddrRange, SystemXBar, L2XBar, MemCtrl, DDR3_1600_8x8,
    X86TimingSimpleCPU,
    BingoPrefetcher,
)

m5.util.addToPath("../../")
from caches import *
from common import SimpleOpts

thispath = os.path.dirname(os.path.realpath(__file__))
default_binary = os.path.join(
    thispath,
    "../../../",
    "tests/test-progs/hello/bin/x86/linux/hello",
)

# CLI options
SimpleOpts.add_option("binary", nargs="?", default=default_binary)

# Pass program argv as a single string, to avoid SimpleOpts/argparse "--" interactions.
# Example: --argv="--mode chase --bytes 256M --iters 2 --seed 1"
SimpleOpts.add_option("--argv", type=str, default="",
                      help="Arguments string passed to the binary (parsed by shlex.split).")

SimpleOpts.add_option("--maxinsts", type=int, default=200000,
                      help="Measurement instructions after warmup (0 = run until program exits)")

# Bingo knobs
SimpleOpts.add_option("--bingo-degree", type=int, default=2)
SimpleOpts.add_option("--warmup-insts", type=int, default=0,
                      help="Warmup instructions before stats reset")
SimpleOpts.add_option("--bingo-history-entries", type=int, default=16384)
SimpleOpts.add_option("--bingo-history-assoc", type=int, default=16)
SimpleOpts.add_option("--bingo-page-buf-entries", type=int, default=8)
SimpleOpts.add_option("--bingo-vote-percent", type=int, default=20)
SimpleOpts.add_option("--enable-bingo", action="store_true", default=False,
                      help="Enable Bingo prefetcher on L2")

args = SimpleOpts.parse_args()

system = System()

system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]

system.cpu = X86TimingSimpleCPU()

system.cpu.icache = L1ICache(args)
system.cpu.dcache = L1DCache(args)

system.cpu.icache.connectCPU(system.cpu)
system.cpu.dcache.connectCPU(system.cpu)

system.l2bus = L2XBar()
system.cpu.icache.connectBus(system.l2bus)
system.cpu.dcache.connectBus(system.l2bus)

system.l2cache = L2Cache(args)
if args.enable_bingo:
    system.l2cache.prefetcher = BingoPrefetcher(
        degree=args.bingo_degree,
        history_entries=args.bingo_history_entries,
        history_assoc=args.bingo_history_assoc,
        page_buf_entries=args.bingo_page_buf_entries,
        vote_percent=args.bingo_vote_percent,

        on_miss=True,
        on_inst=False,
        on_write=False,
        on_read=True,
        on_data=True,
    )

system.l2cache.connectCPUSideBus(system.l2bus)

system.membus = SystemXBar()
system.l2cache.connectMemSideBus(system.membus)

system.cpu.createInterruptController()
system.cpu.interrupts[0].pio = system.membus.mem_side_ports
system.cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
system.cpu.interrupts[0].int_responder = system.membus.mem_side_ports

system.system_port = system.membus.cpu_side_ports

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

# Workload + process
system.workload = SEWorkload.init_compatible(args.binary)

process = Process()
bin_argv = shlex.split(args.argv) if args.argv else []
process.cmd = [args.binary] + bin_argv

system.cpu.workload = process
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()

# max_insts_any_thread is read at instantiate time only, so the warmup and
# measurement windows are set with scheduleInstStop (relative counts).
warmup = int(args.warmup_insts)
maxinsts = int(args.maxinsts)

print("Beginning simulation!")
print("Binary:", args.binary)
print("Binary argv:", process.cmd[1:] if len(process.cmd) > 1 else [])

if warmup > 0:
    print(f"Warmup: {warmup} insts")
    system.cpu.scheduleInstStop(0, warmup, "warmup done")
    exit_event = m5.simulate()
    if exit_event.getCause() != "warmup done":
        print(f"Program ended during warmup: {exit_event.getCause()}")
        raise SystemExit(1)
    m5.stats.reset()

if maxinsts > 0:
    print(f"Measure: {maxinsts} insts")
    system.cpu.scheduleInstStop(0, maxinsts, "measure done")
else:
    print("Measure: run until program exits")

exit_event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
