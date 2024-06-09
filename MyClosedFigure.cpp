
#include "MyClosedFigure.h"
#include <algorithm>

using namespace std;

CMyClosedFigure::CMyClosedFigure()
{
}

CMyClosedFigure::~CMyClosedFigure()
{
}

void CMyClosedFigure::operator=(CMyClosedFigure& other)
{	
	m_PointsArray = other.m_PointsArray;
	m_minX = other.m_minX;
	m_maxX = other.m_maxX;
	m_minY = other.m_minY;
	m_maxY = other.m_maxY;
}

void CMyClosedFigure::operator+=(CMyClosedFigure& other)
{	
	m_PointsArray += other.m_PointsArray;
	if (other.m_minX < m_minX) m_minX = other.m_minX;
	if (other.m_maxX > m_maxX) m_maxX = other.m_maxX;
	if (other.m_minY < m_minY) m_minY = other.m_minY;
	if (other.m_maxY > m_maxY) m_maxY = other.m_maxY;
}
