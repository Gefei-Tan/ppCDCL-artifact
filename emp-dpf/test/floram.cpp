#include "emp-dpf/emp-dpf.h"
#include "emp-tool/emp-tool.h"
#include "emp-dpf/floram.h"
#include <iostream>

using namespace std;
using namespace emp;
using Bit = Bit_T<GbWire>;
using Integer = Integer_T<GbWire>;

const int depth = 20;

FloramMPC<NetIO> * base;

void init_backend(int party) {
    const int threads = 2;
    NetIO ** io = new NetIO *[2];
    for (int i = 0; i < threads; i++) {
        io[i] = new NetIO((party-1) ? "127.0.0.1" : nullptr, 12345+i, true);
    }
    block delta; PRG().random_block(&delta, 1);
    delta ^= makeBlock(0, (getSLSB(delta) << 1) | getLSB(delta));
    if (party == ALICE) delta ^= makeBlock(0, 1);
    else delta ^= makeBlock(0, 3);
    base = new FloramMPC<NetIO>(party, threads, io, delta);
    emp::backend = base;
}

void test_bit() {
    bool b[] = {true, false};
    int p[] = {PUBLIC, ALICE, BOB};
    for(int i = 0; i < 2; ++i)
        for(int j = 0; j < 3; ++j)
            for(int k = 0; k < 2; ++k)
                for (int l= 0; l < 3; ++l)  {
                        Bit b1(b[i], p[j]); Bit b2(b[k], p[l]); Bit b3;
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 & b2;}, b1, b2, b3);
                        bool res = b3.reveal(PUBLIC);
                        if(res != (b[i] and b[k])) {
                            cout<<"AND" <<i<<" "<<j<<" "<<k<<" "<<l<<" "<<res<<endl;
                            error("test AND error!");
                        }
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 & b1;}, b1, b2, b3);
                        res = b3.reveal(PUBLIC);
                        if (res != b[i]) {
                            cout<<"AND" <<i<<" "<<j<<res<<endl;
                            error("test AND error!");
                        }
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 & (!b1);}, b1, b2, b3);
                        res = b3.reveal(PUBLIC);
                        if (res) {
                            cout<<"AND" <<i<<" "<<j<<res<<endl;
                            error("test AND error!");
                        }
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 ^ b2;}, b1, b2, b3);
                        res = b3.reveal(PUBLIC);
                        if(res != (b[i] xor b[k])) {
                            cout <<"XOR"<<i<<" "<<j<<" "<<k<<" "<<l<< " " <<res<<endl;
                            error("test XOR error!");
                        }
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 ^ b1;}, b1, b2, b3);
                        res = b3.reveal(PUBLIC);
                        if (res) {
                            cout<<"XOR" <<i<<" "<<j<<res<<endl;
                            error("test XOR error!");
                        }
                        base->execute([](Bit& b1, Bit& b2, Bit& b3) {b3 = b1 ^ (!b1);}, b1, b2, b3);
                        res = b3.reveal(PUBLIC);
                        if (!res) {
                            cout<<"XOR" <<i<<" "<<j<<res<<endl;
                            error("test XOR error!");
                        }
                    }
    std::cout << "test bit success!" << std::endl;
}

template<typename Op, typename Op2>
void test_int(int party, int range1 = 1<<25, int range2 = 1<<25, bool check = true, int runs = 10) {
    block seed = makeBlock(0,1);
    PRG prg( &seed );
    for(int i = 0; i < runs; ++i) {
        long long ia, ib;
        prg.random_data(&ia, 8);
        prg.random_data(&ib, 8);
        ia %= range1;
        ib %= range2;
        while( Op()(int(ia), int(ib)) != Op()(ia, ib) ) {
            prg.random_data(&ia, 8);
            prg.random_data(&ib, 8);
            ia %= range1;
            ib %= range2;
        }	

        Integer a(32, ia, ALICE);
        Integer b(32, ib, BOB);

        Integer res;

        if (!check) {
            base->execute(
                [](Integer& a, Integer& b, Integer& res, int runs) { for(int i = 0; i < runs; ++i) res = Op2()(a,b); }, 
                a, b, res, runs
            );
            break;
        } else {
            base->execute(
                [](Integer& a, Integer& b, Integer& res) { res = Op2()(a,b); }, 
                a, b, res
            );
            if (res.reveal<int32_t>(PUBLIC) != Op()(ia,ib)) {
                std::cout << ia << " " << ib << std::endl;
                cout << a.reveal<int32_t>()<<endl;
                cout << b.reveal<int32_t>()<<endl;
                cout << ia <<"\t"<<ib<<"\t"<<Op()(ia,ib)<<"\t"<<res.reveal<int32_t>(PUBLIC)<<endl;
            }
            assert(res.reveal<int32_t>(PUBLIC) == Op()(ia,ib));
        }
    }
    cout << typeid(Op2).name()<<"\t\t\tDONE"<<endl;
}

void test_dpf(int sz=5, int a=3, int b=2) {
    std::cout << "DPF testing results:" << std::endl;
    Integer alpha(sz, a, PUBLIC);
    Integer beta(128, b, PUBLIC);
    block *r, beta_value = makeBlock(0, b);
    base->DPF(alpha, beta, 5, &r);
    base->direct_open(r, 1<<(sz));
    for (int i = 0; i < 1<<sz; i++) {
        if (i != a && !cmpBlock(&r[i], &zero_block, 1)) {
            cout << "r[" <<  i << "]: " << r[i] << endl;
            error("DPF error at zero!");
        } if (i == a && !cmpBlock(&r[i], &beta_value, 1)) {
            error("DPF error at alpha/beta");
        } else if (i == a) {
            cout << "r[" <<  i << "]: " << r[i] << endl;
        }
    }
}


void output(FloRAM<NetIO> *arr, string name, int sz) {
    for (int i = 0; i < sz; i++) {
        Integer value;
        arr->read(i, value);
        cout << name << "[" << i << "]: ";
        cout << value.reveal<int64_t>(PUBLIC) << endl;
    }
}

void test_oram(int sz = 10) {
    std::cout << "ORAM testing results:" << std::endl;
    int bit = 3, bit_z = 6;
    cout << "set x[i]=i, y[i]=2i, z[0]=z[1]=1" << endl;
    Integer *arr_x = new Integer[sz], *arr_y = new Integer[sz], *arr_z = new Integer[sz];
    for (int i = 0; i < sz; i++) {
        arr_x[i] = Integer(1<<bit, i, PUBLIC);
        arr_y[i] = Integer(1<<bit, i<<1, PUBLIC);
        arr_z[i] = Integer(1<<bit_z, 1, PUBLIC);
    }
    FloRAM<NetIO> *x, *y, *z;
    x = new FloRAM<NetIO>(base, depth, bit, -1, arr_x, 0, sz);
    y = new FloRAM<NetIO>(base, depth, bit, -1, arr_y, 0, sz);
    z = new FloRAM<NetIO>(base, depth, bit_z, -1, arr_z, 0, 2);
    
    cout << "x[i]=3*x[i]" << endl;
    for (int i = 0; i < sz; i++) {
        Integer idx(depth, i, PUBLIC);
        x->write(idx, 
            [&](const Integer &in, Integer &out) {out = (in<<1) + in;}
        );
    }
    output(x, "x", sz);

    cout << "y[i]=x[i]-y[i]" << endl;
    for (int i = 0; i < sz; i++) {
        Integer idx(depth, i, PUBLIC); 
        Integer x_i;
        x->read(idx, x_i);
        y->write(idx, 
            [&](const Integer &in, Integer &out) {out = x_i - in;}
        );
    }
    y->refresh(y->ka, y->kb);
    output(y, "y", sz);
    
    cout << "z[i]=z[i-1]+z[i-2]" << endl;
    for (int i = 2; i < sz; i++) {
        Integer z1, z2;
        z->read(i-1, z1);
        z->read(i-2, z2);
        z->write(i, [&](const Integer &in, Integer &out) {out = z1+z2;});
    }
    z->refresh(z->ka, z->kb);
    output(z, "z", sz);
}


int main(int argc, char** argv) {
    int party = atoi(argv[1]);
    init_backend(party);

    auto start = clock_start();

    test_dpf();
    test_oram();

    long long t = time_from(start);
    std::cout << "gates:" << emp::backend->num_and() << "\n"; 
    std::cout << emp::backend->num_and() * 1e6 / t << "\n"; 
    delete emp::backend;
    return 0;
}
