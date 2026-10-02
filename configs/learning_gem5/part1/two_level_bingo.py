# import the m5 (gem5) library created when gem5 is built
import m5
import os
import shlex

from m5.objects import (
    System, Root, Process, SEWorkload,
    SrcClockDomain, VoltageDomain,
    AddrRange, SystemXBar, L2XBar, MemCtrl, DDR3_1600_8x8,
    X86TimingSimpleCPU, X86O3CPU, X86AtomicSimpleCPU,
    BingoPrefetcher, SmsPrefetcher, AMPMPrefetcher, BOPPrefetcher,
    SignaturePathPrefetcher,
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
SimpleOpts.add_option("--cpu", choices=["timing", "o3", "atomic"], default="timing",
                      help="atomic is only for counting instructions quickly")

# Bingo knobs
SimpleOpts.add_option("--fast-forward", type=int, default=0,
                      help="Instructions to run on an atomic CPU (caches warm, "
                           "no timing) before switching to --cpu")
SimpleOpts.add_option("--warmup-insts", type=int, default=0,
                      help="Warmup instructions before stats reset")
SimpleOpts.add_option("--bingo-region-size", type=int, default=2048)
SimpleOpts.add_option("--bingo-history-entries", type=int, default=16384)
SimpleOpts.add_option("--bingo-history-assoc", type=int, default=16)
SimpleOpts.add_option("--bingo-ft-entries", type=int, default=64)
SimpleOpts.add_option("--bingo-at-entries", type=int, default=128)
SimpleOpts.add_option("--bingo-vote-percent", type=int, default=20)
SimpleOpts.add_option("--bingo-max-pf", type=int, default=0,
                      help="Cap on prefetches per trigger (0 = whole footprint, as in the paper)")
SimpleOpts.add_option("--bingo-events", choices=["both", "long", "short"],
                      default="both",
                      help="Ablation: PC+Address only, PC+Offset only, or both")
SimpleOpts.add_option("--sms-ft-size", type=int, default=64,
                      help="gem5 SMS filter/active-generation table size")
SimpleOpts.add_option("--enable-bingo", action="store_true", default=False,
                      help="Same as --pf bingo")
SimpleOpts.add_option("--pf", choices=["none", "bingo", "sms", "ampm", "bop",
                                       "spp"], default="none",
                      help="L2 prefetcher; the others are gem5's built-in "
                           "versions of the paper's competitors")

args = SimpleOpts.parse_args()

system = System()

system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()

ff = args.fast_forward > 0 and args.cpu != "atomic"
system.mem_mode = "atomic" if args.cpu == "atomic" or ff else "timing"
system.mem_ranges = [AddrRange("512MiB")]

detailed = {"o3": X86O3CPU, "atomic": X86AtomicSimpleCPU}.get(
    args.cpu, X86TimingSimpleCPU)
system.cpu = X86AtomicSimpleCPU() if ff else detailed()

system.cpu.icache = L1ICache(args)
system.cpu.dcache = L1DCache(args)

system.cpu.icache.connectCPU(system.cpu)
system.cpu.dcache.connectCPU(system.cpu)

system.l2bus = L2XBar()
system.cpu.icache.connectBus(system.l2bus)
system.cpu.dcache.connectBus(system.l2bus)
if ff:
    # Switching CPUs hands over every port, so the page-table walkers must be
    # connected (SE mode never uses them).
    system.cpu.mmu.connectWalkerPorts(
        system.l2bus.cpu_side_ports, system.l2bus.cpu_side_ports)

system.l2cache = L2Cache(args)
if args.enable_bingo:
    args.pf = "bingo"

# The paper trains every prefetcher on all LLC accesses (hits included), so
# all of them see hits here too.
observe = dict(
    queue_size=64,
    on_miss=False,
    prefetch_on_access=True,
    prefetch_on_pf_hit=False,
    on_inst=False,
    on_write=False,
    on_read=True,
    on_data=True,
)
if args.pf == "bingo":
    system.l2cache.prefetcher = BingoPrefetcher(
        region_size=f"{args.bingo_region_size}B",
        history_entries=args.bingo_history_entries,
        history_assoc=args.bingo_history_assoc,
        ft_entries=args.bingo_ft_entries,
        at_entries=args.bingo_at_entries,
        vote_percent=args.bingo_vote_percent,
        max_prefetches=args.bingo_max_pf,
        use_long_event=args.bingo_events != "short",
        use_short_event=args.bingo_events != "long",
        **observe,
    )
elif args.pf == "sms":
    # 16K-entry history as in the paper's SMS configuration
    system.l2cache.prefetcher = SmsPrefetcher(
        region_size=args.bingo_region_size, pht_size=16384,
        ft_size=args.sms_ft_size, **observe)
elif args.pf == "ampm":
    system.l2cache.prefetcher = AMPMPrefetcher(**observe)
elif args.pf == "bop":
    system.l2cache.prefetcher = BOPPrefetcher(**observe)
elif args.pf == "spp":
    system.l2cache.prefetcher = SignaturePathPrefetcher(**observe)

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

if ff:
    # Same setup as configs/common/Simulation.py; the switched-in CPU takes
    # over the caches and interrupt controller of the atomic one.
    # system is left to Parent.any: assigning it here would make the
    # still-unparented system a child of the CPU
    system.switch_cpu = detailed(switched_out=True, cpu_id=0)
    system.switch_cpu.workload = system.cpu.workload
    system.switch_cpu.clk_domain = system.cpu.clk_domain
    system.switch_cpu.isa = system.cpu.isa
    system.switch_cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()

# max_insts_any_thread is read at instantiate time only, so the warmup and
# measurement windows are set with scheduleInstStop (relative counts).
warmup = int(args.warmup_insts)
maxinsts = int(args.maxinsts)

cpu = system.cpu   # the CPU that is running; changes after fast-forward
print("Beginning simulation!")
print("Binary:", args.binary)
print("Binary argv:", process.cmd[1:] if len(process.cmd) > 1 else [])

if ff:
    print(f"Fast-forward: {args.fast_forward} insts on the atomic CPU")
    system.cpu.scheduleInstStop(0, args.fast_forward, "fast-forward done")
    exit_event = m5.simulate()
    if exit_event.getCause() != "fast-forward done":
        print(f"Program ended during fast-forward: {exit_event.getCause()}")
        raise SystemExit(1)
    m5.switchCpus(system, [(system.cpu, system.switch_cpu)])
    cpu = system.switch_cpu

if warmup > 0:
    print(f"Warmup: {warmup} insts")
    cpu.scheduleInstStop(0, warmup, "warmup done")
    exit_event = m5.simulate()
    if exit_event.getCause() != "warmup done":
        print(f"Program ended during warmup: {exit_event.getCause()}")
        raise SystemExit(1)
    m5.stats.reset()

if maxinsts > 0:
    print(f"Measure: {maxinsts} insts")
    cpu.scheduleInstStop(0, maxinsts, "measure done")
else:
    print("Measure: run until program exits")

exit_event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
